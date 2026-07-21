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
static u64 g_sectors;
static u32 g_tag = 0x554D5343;         /* 'UMSC', incremented per command    */

/* CLEAR_FEATURE(ENDPOINT_HALT) - BOT recovery after a command STALLs an
 * endpoint (the normal error path; without it a re-read just STALLs again). */
static void clear_halt(int ep)
{
    uno_usb_control(g_dev, 0x02 /*host->dev,endpoint*/, 1 /*CLEAR_FEATURE*/,
                    0 /*ENDPOINT_HALT*/, (u16)ep, 0, 0);
}

/* ---- BOT plumbing --------------------------------------------------------- */
#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

static int bot_cmd(const u8 *cb, int cblen, void *data, int dlen, int dir_in)
{
    u8 cbw[31], csw[13];
    int n, tries;
    u32 tag = ++g_tag;
    memset(cbw, 0, sizeof cbw);
    cbw[0]='U'; cbw[1]='S'; cbw[2]='B'; cbw[3]='C';
    cbw[4]=(u8)tag; cbw[5]=(u8)(tag>>8); cbw[6]=(u8)(tag>>16); cbw[7]=(u8)(tag>>24);
    cbw[8]=(u8)dlen; cbw[9]=(u8)(dlen>>8); cbw[10]=(u8)(dlen>>16); cbw[11]=(u8)(dlen>>24);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;                                   /* LUN 0 */
    cbw[14] = (u8)cblen;
    memcpy(cbw + 15, cb, (size_t)cblen);

    if (uno_usb_bulk_out(g_dev, cbw, 31) != 31) return -1;
    if (dlen > 0) {
        if (dir_in) { if (uno_usb_bulk_in(g_dev, data, dlen) < 0) clear_halt(g_in_ep); }
        else        { if (uno_usb_bulk_out(g_dev, data, dlen) != dlen) clear_halt(g_out_ep); }
    }
    /* CSW; if the data phase STALLed the IN endpoint the device parks the CSW
     * behind the halt - clear it and retry once (the standard BOT recovery). */
    for (tries = 0; tries < 2; tries++) {
        n = uno_usb_bulk_in(g_dev, csw, 13);
        if (n == 13) break;
        clear_halt(g_in_ep);
    }
    if (n != 13) return -1;
    if ((u32)(csw[0]|(csw[1]<<8)|(csw[2]<<16)|((u32)csw[3]<<24)) != CSW_SIG) return -1;
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
        if (bot_cmd(cb, 6, 0, 0, 1) == 0) return 0;
        scsi_request_sense();                       /* clear + spin-up wait */
        uno_pc64_delay_ms(50);
    }
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
#define MSC_CHUNK 64u                              /* sectors per transfer     */

static int msc_rw(u64 lba, u32 n, void *buf, int write)
{
    u8 *p = (u8 *)buf;
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
            if (r != 0) return 0;
        }
        lba += c; p += c * 512; n -= c;
    }
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
    static u8 cfg[512];
    int n = uno_usb_get_config(dev, cfg, sizeof cfg);
    int total, i, in_msc = 0, got_in = 0, got_out = 0;
    if (n < 9) return -1;
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

int uno_usbmsc_init(void)
{
    int i, n;
    if (g_bound) return 1;
    if (!uno_xhci_init()) return 0;
    n = uno_xhci_dev_count();
    for (i = 0; i < n; i++) {
        int ifnum = 0, in_ep = 0, out_ep = 0, in_mps = 512, out_mps = 512, cfgval = 1;
        if (find_bot_interface(i, &ifnum, &in_ep, &out_ep, &in_mps, &out_mps, &cfgval) < 0)
            continue;
        g_dev = i; g_ifnum = ifnum;
        if (uno_usb_set_config(g_dev, cfgval) < 0) continue;
        if (uno_usb_setup_bulk(g_dev, in_ep, out_ep, in_mps, out_mps) < 0) continue;
        g_in_ep = in_ep; g_out_ep = out_ep;        /* for BOT halt recovery */
        /* Get Max LUN (optional; many sticks STALL it - ignore the result) */
        { u8 luns = 0;
          uno_usb_control(g_dev, 0xA1, 0xFE, 0, (u16)g_ifnum, &luns, 1); }
        if (scsi_ready() < 0)          { uno_dbg_log("usbmsc: unit never ready"); continue; }
        if (scsi_read_capacity(&g_sectors) < 0) continue;
        {
            uno_bdev d;
            memset(&d, 0, sizeof d);
            d.native = 1;
            d.sectors = g_sectors;
            strcpy(d.name, "usb0");
            d.pci_dev = -1; d.pci_fn = -1;
            d.read = msc_read; d.write = msc_write;
            if (!uno_blk_register(&d)) return 0;
        }
        uno_dbg_log("usbmsc: BOT device up, %llu sectors", g_sectors);
        g_bound = 1;
        return 1;
    }
    return 0;
}
