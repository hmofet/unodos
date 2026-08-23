/* ===========================================================================
 * UnoDOS/pc64 - USB mass storage (Bulk-Only Transport) over the xHCI stack.
 *
 * P4: the missing piece that made detach strand a USB-booted system (F8).
 * After ExitBootServices the firmware's Block IO is gone; this driver speaks
 * BOT/SCSI over uno_usb_* bulk pipes and registers the stick with blkdev, so
 * the boot volume survives detach like an AHCI/NVMe disk does.
 *
 * Scope: one LUN-0 BOT device (the boot stick), 512-byte blocks, READ(10)/
 * WRITE(10) in 32 KiB chunks, TEST-UNIT-READY spin-up retry, REQUEST SENSE on
 * a failed CSW. Follows the USB MSC BOT 1.0 spec; the SCSI subset matches
 * what every flash stick implements. Inert when the xHCI stack is stubbed
 * out (!UNO_XHCI) or while firmware-attached.
 * ======================================================================== */
#include "usbmsc.h"
#include "uno_devmgr.h"
#include "usbboot.h"
#include "blkdev.h"
#include "xhci.h"
#include "uno_debug.h"
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void uno_pc64_delay_ms(int ms);

static int g_dev = -1;                 /* xHCI device index                  */
static int g_ifnum;                    /* interface number (class requests)  */
static int g_in_ep, g_out_ep;          /* bulk endpoint addresses            */
static int g_bound;
static const char *g_why = "not attempted";
static const char *g_boot_why;         /* the targeted boot device's reason  */
static u64 g_sectors;
static u32 g_tag = 0x554D5343;         /* 'UMSC', incremented per command    */

/* CLEAR_FEATURE(ENDPOINT_HALT) - BOT recovery after a command STALLs an
 * endpoint (the normal error path; without it a re-read just STALLs again). */
static void clear_halt(int ep)
{
    uno_usb_control(g_dev, 0x02 /*host->dev,endpoint*/, 1 /*CLEAR_FEATURE*/,
                    0 /*ENDPOINT_HALT*/, (u16)ep, 0, 0);
}

/* ---- bring-up trace ------------------------------------------------------
 * Post-detach there is no console, no log file (the volume we would write it
 * to is the one that just failed to come back) and no way to retry. On the
 * bench that leaves exactly one channel: QEMU's debugcon, and a serial line on
 * metal. Compiled out unless -DUNO_DBGCON, which is also the only build where
 * touching port 0x402 is safe. */
#ifdef UNO_DBGCON
static void tr(const char *s)
{
    while (*s) { __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)*s++), "Nd"((unsigned short)0x402)); }
}
static void tri(const char *s, long v)
{
    char b[24]; int i = 0, neg = v < 0;
    unsigned long u = (unsigned long)(neg ? -v : v);
    tr(s);
    do { b[i++] = (char)('0' + (u % 10)); u /= 10; } while (u && i < 20);
    if (neg) b[i++] = '-';
    while (i) { char c = b[--i];
        __asm__ volatile ("outb %0, %1" : : "a"((unsigned char)c), "Nd"((unsigned short)0x402)); }
    tr("\n");
}
#else
#define tr(s)      ((void)0)
#define tri(s, v)  ((void)0)
#endif

/* ---- BOT plumbing --------------------------------------------------------- */
#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

/* DMA buffers, never the stack.
 *
 * xhci.c puts the caller's pointer straight into the TRB as the controller's
 * DMA address, so every buffer handed to uno_usb_bulk_* is one the hardware
 * writes to directly. A local array makes that a DMA into the firmware-
 * provided stack, at whatever address and alignment the current build's frame
 * layout happens to produce - which is how this worked in one build and hung
 * in another with identical logic. Static, cache-line aligned, permanent.
 * (Same lesson as the AHCI/IoAlign sector buffers in the M1 storage work.) */
static __attribute__((aligned(64))) u8 g_cbw[31];
static __attribute__((aligned(64))) u8 g_csw[13];
#define MSC_CHUNK 64u                              /* sectors per transfer     */
static __attribute__((aligned(64))) u8 g_bounce[MSC_CHUNK * 512];

static int bot_cmd(const u8 *cb, int cblen, void *data, int dlen, int dir_in)
{
    u8 *cbw = g_cbw, *csw = g_csw;
    int n = 0, tries, short_data = 0;
    u32 tag = ++g_tag;
    memset(cbw, 0, 31);
    cbw[0]='U'; cbw[1]='S'; cbw[2]='B'; cbw[3]='C';
    cbw[4]=(u8)tag; cbw[5]=(u8)(tag>>8); cbw[6]=(u8)(tag>>16); cbw[7]=(u8)(tag>>24);
    cbw[8]=(u8)dlen; cbw[9]=(u8)(dlen>>8); cbw[10]=(u8)(dlen>>16); cbw[11]=(u8)(dlen>>24);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;                                   /* LUN 0 */
    cbw[14] = (u8)cblen;
    memcpy(cbw + 15, cb, (size_t)cblen);

    if (uno_usb_bulk_out(g_dev, cbw, 31) != 31) { tri("usbmsc: CBW out failed, op=", cb[0]); return -1; }
    if (dlen > 0) {
        /* the data phase rides the bounce too - callers pass FAT cache lines,
         * SCSI sense arrays and whatever else, none of it DMA-fit by luck */
        int got;
        if (dlen > (int)sizeof g_bounce) return -1;
        if (dir_in) {
            got = uno_usb_bulk_in(g_dev, g_bounce, dlen);
            if (got < dlen) { tri("usbmsc: short IN, op=", cb[0]); tri("  wanted=", dlen); tri("  got=", got); }
            /* A short or failed data phase is NOT success. Falling through to
             * the CSW here is how a read of nothing became 512 bytes of zeros
             * that FAT then mounted as an empty volume. */
            if (got < dlen) { clear_halt(g_in_ep); short_data = 1; }
            if (got > 0)    memcpy(data, g_bounce, (size_t)got);
        } else {
            memcpy(g_bounce, data, (size_t)dlen);
            got = uno_usb_bulk_out(g_dev, g_bounce, dlen);
            if (got != dlen) { clear_halt(g_out_ep); short_data = 1; }
        }
    }
    /* CSW.  Two things go wrong here and both used to pass silently:
     *   - the data phase STALLed, so the device parks the CSW behind the halt
     *     (clear it and read again - the standard BOT recovery), and
     *   - the CSW that arrives belongs to an EARLIER command. BOT tags every
     *     command precisely so a desynchronised pipe can be detected, and
     *     without the check a stale CSW makes this command "succeed" while the
     *     caller reads whatever the previous one left in the buffer. Drain
     *     until the tag matches, then continue. */
    for (tries = 0; tries < 2; tries++) {
        n = uno_usb_bulk_in(g_dev, csw, 13);
        if (n != 13) { tri("usbmsc: CSW read n=", n); clear_halt(g_in_ep); continue; }
        if ((u32)(csw[0]|(csw[1]<<8)|(csw[2]<<16)|((u32)csw[3]<<24)) != CSW_SIG)
            continue;                              /* not a CSW at all: drain  */
        if ((u32)(csw[4]|(csw[5]<<8)|(csw[6]<<16)|((u32)csw[7]<<24)) == tag)
            break;                                 /* ours                     */
    }
    if (n != 13) return -1;
    if ((u32)(csw[0]|(csw[1]<<8)|(csw[2]<<16)|((u32)csw[3]<<24)) != CSW_SIG) return -1;
    if ((u32)(csw[4]|(csw[5]<<8)|(csw[6]<<16)|((u32)csw[7]<<24)) != tag)     return -1;
    if (csw[12] == 0 && short_data) return 1;      /* device says ok, we know better */
    return csw[12];                                /* 0 ok, 1 failed, 2 phase */
}

static int scsi_request_sense(void)
{
    u8 cb[6] = { 0x03, 0, 0, 0, 18, 0 }, sense[18];
    return bot_cmd(cb, 6, sense, 18, 1);
}

static int scsi_ready(void)
{
    u8 cb[6] = { 0x00, 0, 0, 0, 0, 0 };            /* TEST UNIT READY */
    int t;
    for (t = 0; t < 20; t++) {
        if (bot_cmd(cb, 6, 0, 0, 1) == 0) {
            /* How many rounds it took is the difference between a stick that
             * was ready and one that needed nursing - and each round can cost
             * multi-second bulk timeouts, which is the shape of a machine
             * that looks bricked while it is in fact still working. */
            if (t) uno_dbg_log("usbmsc: unit ready after %d retr%s",
                               t, t == 1 ? "y" : "ies");
            return 0;
        }
        scsi_request_sense();                       /* clear + spin-up wait */
        uno_pc64_delay_ms(50);
    }
    uno_dbg_log("usbmsc: unit NEVER became ready (20 rounds of TEST UNIT READY)");
    return -1;
}

static int scsi_read_capacity(u64 *sectors)
{
    u8 cb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 }, cap[8];
    if (bot_cmd(cb, 10, cap, 8, 1) != 0) return -1;
    {
        u32 last = ((u32)cap[0]<<24)|((u32)cap[1]<<16)|((u32)cap[2]<<8)|cap[3];
        u32 bsz  = ((u32)cap[4]<<24)|((u32)cap[5]<<16)|((u32)cap[6]<<8)|cap[7];
        if (bsz != 512) {
            uno_dbg_log("usbmsc: block size %u unsupported (512 only)", bsz);
            return -1;
        }
        *sectors = (u64)last + 1;
    }
    return 0;
}

/* ---- blkdev backend -------------------------------------------------------- */
static int msc_rw(u64 lba, u32 n, void *buf, int write)
{
    u8 *p = (u8 *)buf;
    /* THE FIRST WRITE IS THE ONE THAT MATTERS.
     *
     * Everything before it - enumeration, SET_CONFIGURATION, spin-up, READ
     * CAPACITY, the LBA-0 proof read - is a read path. On the Surface Laptop
     * Go the takeover gets all of that right and the machine then stops
     * somewhere in the window whose first disk access is the telemetry
     * WRITE(10). If the log ends on this line, the write never came back and
     * the bulk-out/CSW path is the suspect; if the "ok" line follows, it is
     * not. One boot, one bit of information, and the line costs nothing
     * afterwards because it announces only once. */
    static int announced;
    int first = 0;
    if (write && !announced) {
        announced = 1; first = 1;
        uno_dbg_log("usbmsc: FIRST WRITE(10) lba=%llu n=%u - if the log ends "
                    "here, it never returned", (unsigned long long)lba, n);
    }
    while (n) {
        u32 c = n > MSC_CHUNK ? MSC_CHUNK : n;
        u8 cb[10];
        int r;
        cb[0] = write ? 0x2A : 0x28;               /* WRITE(10)/READ(10)      */
        cb[1] = 0;
        cb[2]=(u8)(lba>>24); cb[3]=(u8)(lba>>16); cb[4]=(u8)(lba>>8); cb[5]=(u8)lba;
        cb[6] = 0;
        cb[7]=(u8)(c>>8); cb[8]=(u8)c;
        cb[9] = 0;
        r = bot_cmd(cb, 10, p, (int)(c * 512), !write);
        if (r != 0) {
            scsi_request_sense();
            r = bot_cmd(cb, 10, p, (int)(c * 512), !write);   /* one retry */
            if (r != 0) {
                if (first) uno_dbg_log("usbmsc: FIRST WRITE(10) FAILED (bot rc=%d)", r);
                return 0;
            }
        }
        lba += c; p += c * 512; n -= c;
    }
    if (first) uno_dbg_log("usbmsc: first WRITE(10) ok - the write path works");
    return 1;
}

static int msc_read(uno_bdev *d, u64 lba, u32 n, void *buf)
{ (void)d; return msc_rw(lba, n, buf, 0); }
static int msc_write(uno_bdev *d, u64 lba, u32 n, const void *buf)
{ (void)d; return msc_rw(lba, n, (void *)buf, 1); }

/* ---- bring-up --------------------------------------------------------------
 * Find a BOT interface (class 08 / subclass 06 / protocol 0x50) in the config
 * descriptor of any enumerated xHCI device, claim its bulk pipes, and
 * register the medium. Composite devices report class 0 at the device level,
 * so the interface descriptor is the only truth. */
static int find_bot_interface(int dev, int *ifnum, int *in_ep, int *out_ep,
                              int *in_mps, int *out_mps, int *cfgval)
{
    static __attribute__((aligned(64))) u8 cfg[512];
    int n = uno_usb_get_config(dev, cfg, sizeof cfg);
    int total, i, in_msc = 0, got_in = 0, got_out = 0;
    if (n < 9) return -2;                          /* descriptor fetch failed */
    *cfgval = cfg[5];
    total = cfg[2] | (cfg[3] << 8); if (total > (int)sizeof cfg) total = n;
    for (i = 0; i + 2 <= total; ) {
        int len = cfg[i], type = cfg[i+1];
        if (len < 2) break;
        if (type == 0x04 && i + 9 <= total) {      /* INTERFACE (9-byte descr) */
            if (in_msc && got_in && got_out) return 0;
            in_msc = (cfg[i+5] == 0x08 && cfg[i+6] == 0x06 && cfg[i+7] == 0x50);
            if (in_msc) { *ifnum = cfg[i+2]; got_in = got_out = 0; }
        } else if (type == 0x05 && in_msc && i + 6 <= total) {   /* ENDPOINT */
            int addr = cfg[i+2], attr = cfg[i+3];
            int mps  = cfg[i+4] | (cfg[i+5] << 8);
            if ((attr & 0x03) == 0x02) {
                if ((addr & 0x80) && !got_in)   { *in_ep = addr;  *in_mps = mps;  got_in = 1; }
                if (!(addr & 0x80) && !got_out) { *out_ep = addr; *out_mps = mps; got_out = 1; }
            }
        }
        i += len;
    }
    return (in_msc && got_in && got_out) ? 0 : -1;
}

int uno_usbmsc_supported(void) { return uno_xhci_supported(); }

/* Bind the boot stick, not "whichever mass-storage device enumerated first".
 *
 * On a USB boot the whole point of this driver is to get the SYSTEM volume
 * back, and usbboot identified that device by USB id while the firmware could
 * still tell us which one it was. So: one pass restricted to that id, then a
 * second unrestricted pass for every other case (an internal-disk boot with a
 * data stick attached, or a firmware that gave us no usable identity). */
static int try_bind(int i, int want_boot);

/* ---- unodevices registry adoption (phase 3) -------------------------------
 * Mass storage, SCSI transparent, Bulk-Only Transport - the one interface
 * triple this driver speaks. Record-only for the same reason as usbhid: this
 * is the path that reclaims the boot volume at detach, and past
 * ExitBootServices there is no way back. */
static int msc_probe(uno_device *d) { (void)d; return 1; }
static const uno_match msc_match[] = {
    { UNO_MATCH_USB_IF, 0, 0, 0x08, 0x06, 0x50, 1 },  /* MSC / SCSI / BOT */
    { UNO_MATCH_END,    0, 0, 0,    0,    0,    0 }
};
static const uno_driver msc_drv = {
    "usbmsc", UNO_BUS_USB, UNO_DEVMGR_API, msc_match, msc_probe, 0
};
UNO_DRIVER(msc_drv);

int uno_usbmsc_init(void)
{
    unsigned short want_vid = 0, want_pid = 0;
    int i, n, pass, targeted;
    if (g_bound) return 1;
    if (!uno_xhci_init()) { g_why = "xHCI controller did not come up"; return 0; }
    targeted = uno_usbboot_target(&want_vid, &want_pid);
    n = uno_xhci_dev_count();
    uno_dbg_log("usbmsc: binding, %d enumerated device(s), target %s %04x:%04x",
                n, targeted ? "=" : "(none - unrestricted scan)",
                want_vid, want_pid);
    if (!n) { g_why = "xHCI enumerated no devices"; return 0; }
    g_why = "no mass-storage device among the enumerated ones";
    for (pass = targeted ? 0 : 1; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            const uno_usb_dev *d = uno_xhci_dev(i);
            if (pass == 0 && (!d || d->vendor != want_vid || d->product != want_pid))
                continue;
            if (try_bind(i, pass == 0)) return 1;
        }
        /* Keep the BOOT device's reason. Pass 1 walks every device including
         * keyboards and hubs, whose "not mass storage" is true and useless -
         * it must not overwrite the account of why the stick itself failed. */
        if (pass == 0 && g_why) { g_boot_why = g_why; }
    }
    if (g_boot_why) g_why = g_boot_why;
    return 0;
}

#define FAIL(msg) do { g_why = (msg); return 0; } while (0)

static int try_bind(int i, int want_boot)
{
    int ifnum = 0, in_ep = 0, out_ep = 0, in_mps = 512, out_mps = 512, cfgval = 1;
    int f = find_bot_interface(i, &ifnum, &in_ep, &out_ep, &in_mps, &out_mps, &cfgval);
    if (f == -2) FAIL("GET_DESCRIPTOR(config) failed");
    if (f <  0)  FAIL("no BOT interface in the config descriptor");
    g_dev = i; g_ifnum = ifnum;
    if (uno_usb_set_config(g_dev, cfgval) < 0) FAIL("SET_CONFIGURATION failed");
    tri("usbmsc: binding dev=", i); tri("  bulk in ep=", in_ep); tri("  bulk out ep=", out_ep);
    tri("  max packet=", in_mps);      /* 1024 => SuperSpeed; see USB.md */
    if (uno_usb_setup_bulk(g_dev, in_ep, out_ep, in_mps, out_mps) < 0)
        FAIL("bulk endpoints would not configure");
    g_in_ep = in_ep; g_out_ep = out_ep;            /* for BOT halt recovery */
    /* Get Max LUN (optional; many sticks STALL it - ignore the result) */
    { u8 luns = 0;
      uno_usb_control(g_dev, 0xA1, 0xFE, 0, (u16)g_ifnum, &luns, 1); }
    if (scsi_ready() < 0)          { uno_dbg_log("usbmsc: unit never ready");
                                     FAIL("unit never became ready"); }
    if (scsi_read_capacity(&g_sectors) < 0) FAIL("READ CAPACITY failed");
    /* Prove the data path before handing this device to the filesystem. A
     * bind that answers descriptors but cannot move a sector is worse than no
     * bind at all: blkdev would register it, FAT would mount nothing, and the
     * system volume would be "gone" with no explanation. LBA 0 of anything we
     * boot from is a partition sector, so the signature is a free check. */
    {
        static u8 lba0[512];
        if (!msc_rw(0, 1, lba0, 0))                  FAIL("first READ(10) failed");
        if (lba0[510] != 0x55 || lba0[511] != 0xAA)  FAIL("no partition signature at LBA 0");
    }
    {
        uno_bdev d;
        memset(&d, 0, sizeof d);
        d.native  = 1;
        d.sectors = g_sectors;
        strcpy(d.name, "usb0");
        d.pci_dev = -1; d.pci_fn = -1;
        /* The storage safety gate refuses to wipe the disk we booted from, and
         * after detach fw_scan's is_boot marking is gone with the firmware -
         * so carry it across here, on the one device we can prove is it. */
        d.is_boot = want_boot;
        d.read = msc_read; d.write = msc_write;
        if (!uno_blk_register(&d)) FAIL("block-device registry full");
    }
    uno_dbg_log("usbmsc: BOT device up, %llu sectors, boot=%d", g_sectors, want_boot);
    g_why = "up";
    g_bound = 1;
    return 1;
}
#undef FAIL

const char *uno_usbmsc_why(void) { return g_why; }
