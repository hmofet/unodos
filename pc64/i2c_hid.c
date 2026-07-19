/* ===========================================================================
 * UnoDOS/pc64 - native I2C-HID trackpad driver (see i2c_hid.h).
 *
 * Gated behind UNO_I2C_TRACKPAD. When off, this compiles to inert stubs so the
 * verified build is unaffected.
 * ======================================================================== */
#include "i2c_hid.h"

#ifndef UNO_I2C_TRACKPAD

int uno_i2c_hid_init(void) { return 0; }
int uno_i2c_hid_poll(int *ax, int *ay, int *b) { (void)ax;(void)ay;(void)b; return 0; }
int uno_i2c_hid_kbd_poll(uno_i2c_key_fn e, void *c) { (void)e;(void)c; return 0; }
int uno_i2c_hid_kbd_present(void) { return 0; }
int uno_i2c_hid_dump(unsigned char *o, int m) { (void)o;(void)m; return 0; }
int uno_i2c_hid_present(void) { return 0; }
void uno_i2c_hid_status(int *nb, int *nc, int *pr, int *ad, int *pa)
{ if (nb)*nb=0; if (nc)*nc=0; if (pr)*pr=0; if (ad)*ad=0; if (pa)*pa=0; }
void uno_i2c_hid_diag(int *sa, unsigned *ab) { if (sa)*sa=0; if (ab)*ab=0; }

#else  /* ================= UNO_I2C_TRACKPAD enabled ==================== */

#include "pc64_pci.h"
#include "hid_kbd.h"
#include <stdint.h>

/* ===========================================================================
 * SELF-CONFIGURING bring-up. Rather than depend on ACPI/AML constants, we
 * enumerate the machine: scan every PCI-visible Intel LPSS I2C controller, and
 * on each probe the handful of slave-address x HID-descriptor-register
 * combinations real HID devices use, validating each by the HID descriptor's
 * signature (wHIDDescLength==30, bcdVersion==0x0100). Each valid device is
 * classified from its report descriptor as a POINTER (Generic-Desktop X/Y) or
 * a KEYBOARD (Keyboard/Keypad page) and kept in the matching slot - so a laptop
 * whose keyboard AND touchpad are both I2C-HID (the Surface) gets both, no
 * hand-supplied constants. An override (-DI2C_HID_BASE=...) is still honoured
 * for a controller hidden from PCI (ACPI-only mode).
 * ======================================================================== */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* candidates a Windows-Precision-Touchpad / HID-over-I2C pad answers on */
static const u8  kAddrs[]   = { 0x2c, 0x15, 0x2a, 0x10, 0x20, 0x05, 0x06, 0x07, 0x25, 0x2d };
static const u16 kDescRegs[] = { 0x0020, 0x0001, 0x0000 };

/* ---- DesignWare (Synopsys DW_apb_i2c) register offsets ------------------ */
#define DW_IC_CON           0x00
#define DW_IC_TAR           0x04
#define DW_IC_DATA_CMD      0x10
#define DW_IC_FS_SCL_HCNT   0x1c
#define DW_IC_FS_SCL_LCNT   0x20
#define DW_IC_INTR_MASK     0x30
#define DW_IC_CLR_INTR      0x40
#define DW_IC_CLR_TX_ABRT   0x54
#define DW_IC_ENABLE        0x6c
#define DW_IC_STATUS        0x70
#define DW_IC_TXFLR         0x74
#define DW_IC_RXFLR         0x78
#define DW_IC_TX_ABRT_SOURCE 0x80
#define DW_IC_ENABLE_STATUS 0x9c
#define DW_IC_COMP_TYPE     0xfc    /* DesignWare signature register */
#define DW_COMP_TYPE_VALUE  0x44570140u   /* "DW" identity - guards a bad BAR */
#define LPSS_RESETS         0x804   /* Intel LPSS private: controller reset */

#define DW_CON_MASTER       0x01
#define DW_CON_SPEED_FAST   0x04
#define DW_CON_RESTART_EN   0x20
#define DW_CON_SLAVE_DIS    0x40

#define DW_STATUS_TFNF      0x02    /* tx fifo not full */
#define DW_STATUS_TFE       0x04    /* tx fifo empty */
#define DW_STATUS_RFNE      0x08    /* rx fifo not empty */

#define DW_CMD_READ         (1u << 8)
#define DW_CMD_STOP         (1u << 9)
#define DW_CMD_RESTART      (1u << 10)

static volatile u8 *g_i2c;         /* controller MMIO base of the CURRENT xfer */

/* one discovered HID device (pointer or keyboard) */
typedef struct {
    volatile u8 *i2c;              /* its controller base */
    u8  addr;                      /* its slave address    */
    int present;
    u16 input_reg, max_input, cmd_reg, rd_reg, rd_len;
    /* pointer parse results: bit offsets + sizes + logical maxima for scaling.
     * parsed==0 -> poll falls back to the common WPT/boot-mouse shape. */
    int parsed;
    u8  report_id;
    int x_off, x_bits, x_lmax;
    int y_off, y_bits, y_lmax;
    int tip_off;                   /* tip-switch / button-1 bit, -1 if none */
    /* keyboard */
    int is_kbd;
    u8  kbd_report_id;             /* report ID prefixing kbd input, 0 if none */
    hid_kbd_state kbd;
} hiddev;

static hiddev g_ptr;               /* trackpad / mouse slot   */
static hiddev g_kbd;               /* keyboard slot           */
static u8  g_report[64];           /* last pointer report (for _dump)         */
static int g_report_len;

static u32 r32(u32 o) { return *(volatile u32 *)(g_i2c + o); }
static void w32(u32 o, u32 v) { *(volatile u32 *)(g_i2c + o) = v; }
static void spin(volatile int n) { while (n-- > 0) __asm__ volatile (""); }

/* probe diagnostics (surfaced by uno_i2c_hid_diag for the System readout):
 * did any transfer return bytes (bus + a device answered), and the last
 * TX_ABRT_SOURCE. Lets the next metal readout distinguish a dead bus (all
 * timeout, abrt 0) from NAKs (abrt bit7 set) from a bad descriptor. */
static int g_saw_bytes;
static u32 g_last_abrt;

/* ---- DesignWare master: write wbuf then (repeated-start) read rbuf ------- */
static int dw_wr_rd(u8 addr, const u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
    int i, got = 0, tries;

    /* disable, program target + fast-mode clocks, re-enable */
    w32(DW_IC_ENABLE, 0);
    for (tries = 100000; (r32(DW_IC_ENABLE_STATUS) & 1) && tries; tries--) spin(4);
    w32(DW_IC_CON, DW_CON_MASTER | DW_CON_SPEED_FAST |
                   DW_CON_RESTART_EN | DW_CON_SLAVE_DIS);
    w32(DW_IC_TAR, addr);
    /* ~400 kHz off a 133 MHz LPSS clock (typical); firmware often preset these
       already - conservative values that work on Intel LPSS */
    w32(DW_IC_FS_SCL_HCNT, 0x48);
    w32(DW_IC_FS_SCL_LCNT, 0x4f);
    w32(DW_IC_INTR_MASK, 0);
    w32(DW_IC_ENABLE, 1);
    (void)r32(DW_IC_CLR_TX_ABRT);

    /* push write bytes */
    for (i = 0; i < wlen; i++) {
        for (tries = 100000; !(r32(DW_IC_STATUS) & DW_STATUS_TFNF) && tries; tries--) spin(4);
        w32(DW_IC_DATA_CMD, wbuf[i]);
    }
    /* push read commands (RESTART on first, STOP on last) */
    for (i = 0; i < rlen; i++) {
        u32 cmd = DW_CMD_READ;
        if (i == 0) cmd |= DW_CMD_RESTART;
        if (i == rlen - 1) cmd |= DW_CMD_STOP;
        for (tries = 100000; !(r32(DW_IC_STATUS) & DW_STATUS_TFNF) && tries; tries--) spin(4);
        w32(DW_IC_DATA_CMD, cmd);
    }
    /* drain read data */
    for (i = 0; i < rlen; i++) {
        for (tries = 200000; !(r32(DW_IC_STATUS) & DW_STATUS_RFNE) && tries; tries--) spin(4);
        if (!(r32(DW_IC_STATUS) & DW_STATUS_RFNE)) break;   /* timeout */
        rbuf[got++] = (u8)(r32(DW_IC_DATA_CMD) & 0xFF);
    }
    { u32 ab = r32(DW_IC_TX_ABRT_SOURCE); if (ab) g_last_abrt = ab;
      if (got > 0) g_saw_bytes = 1;
      if (ab) { (void)r32(DW_IC_CLR_TX_ABRT); return -1; } }
    return got;
}

/* write a 16-bit register address (LE), no read - for HID commands */
static int dw_write(u8 addr, const u8 *buf, int len)
{
    return dw_wr_rd(addr, buf, len, 0, 0) < 0 ? -1 : 0;
}
/* set the read pointer to a 2-byte register, then read len bytes */
static int dw_reg_read(u8 addr, u16 reg, u8 *out, int len)
{
    u8 w[2]; w[0] = reg & 0xFF; w[1] = reg >> 8;
    return dw_wr_rd(addr, w, 2, out, len);
}

/* ---- discovery: every PCI-visible Intel LPSS I2C controller -------------- */
static int list_bars(u64 *bars, int max)
{
    int n = 0;
#ifdef I2C_HID_BASE
    if ((u64)(I2C_HID_BASE) && n < max) bars[n++] = (u64)(I2C_HID_BASE);  /* override */
#endif
    { pci_dev d;                              /* first class-0C80 8086 match */
      if (pci_find_class(0x0C, 0x80, &d) && d.vendor == 0x8086 && n < max) {
          pci_enable_bus_master(&d); bars[n++] = pci_bar(&d, 0);
      } }
    /* Intel LPSS I2C sit at fixed BDFs; probe them too so a pad on a later
       controller (not the first class match) is still found. */
    { static const int devs[] = { 0x15, 0x19, 0x1e };
      int di, fn;
      for (di = 0; di < 3; di++) for (fn = 0; fn < 4 && n < max; fn++) {
          pci_dev d; u32 id, cls; u64 b; int j, dup = 0;
          d.bus = 0; d.dev = devs[di]; d.fn = fn; d.vendor = d.device = 0;
          id = pci_cfg_read32(&d, 0x00);
          if (id == 0xFFFFFFFFu || (id & 0xFFFF) != 0x8086) continue;
          cls = pci_cfg_read32(&d, 0x08) >> 16;         /* subclass|class<<8 */
          if ((cls & 0xFFFF) != 0x0C80) continue;       /* serial bus / other */
          d.vendor = id & 0xFFFF; d.device = id >> 16;
          pci_enable_bus_master(&d);
          b = pci_bar(&d, 0);
          for (j = 0; j < n; j++) if (bars[j] == b) dup = 1;
          if (b && !dup) bars[n++] = b;
      } }
    return n;
}

/* read + validate the HID descriptor at (addr, descReg); fills the register
 * map of `dev` on success. Validation by signature = self-configuring probe. */
static int try_hid(hiddev *dev, u8 addr, u16 descReg)
{
    u8 d[30];
    int n = dw_reg_read(addr, descReg, d, sizeof d);
    if (n < 30) return 0;
    if ((d[0] | (d[1] << 8)) != 30)      return 0;   /* wHIDDescLength */
    if ((d[2] | (d[3] << 8)) != 0x0100)  return 0;   /* bcdVersion 1.00 */
    dev->i2c       = g_i2c;
    dev->addr      = addr;
    dev->rd_len    = (u16)(d[4]  | (d[5]  << 8));
    dev->rd_reg    = (u16)(d[6]  | (d[7]  << 8));
    dev->input_reg = (u16)(d[8]  | (d[9]  << 8));
    dev->max_input = (u16)(d[10] | (d[11] << 8));
    dev->cmd_reg   = (u16)(d[16] | (d[17] << 8));
    if (dev->max_input == 0 || dev->max_input > sizeof g_report) dev->max_input = sizeof g_report;
    return 1;
}

static void hid_power_reset(hiddev *dev)
{
    u8 c[4];
    c[0] = dev->cmd_reg & 0xFF; c[1] = dev->cmd_reg >> 8; c[2] = 0x00; c[3] = 0x08;  /* power on */
    dw_write(dev->addr, c, 4);
    c[2] = 0x00; c[3] = 0x01;                                                        /* reset */
    dw_write(dev->addr, c, 4);
    { int t = 500000; while (t--) spin(4); }         /* let reset settle */
}

/* ---- HID report-descriptor parser --------------------------------------- *
 * Walk the item stream tracking usage page, report size/count and the running
 * bit offset. For a POINTER: record Generic-Desktop X (page 1 usage 0x30), Y
 * (0x31) and the tip switch (Digitizer 0x0D/0x42 or Button-1). For a KEYBOARD:
 * note any Input item on the Keyboard/Keypad page (0x07) and its report ID.
 * One device can only be one or the other in the slot it lands in. */
static void parse_rd(hiddev *dev, const u8 *rd, int len)
{
    int i = 0, page = 0, rsize = 0, rcount = 0, bitpos = 0, lmax = 0, cur_id = 0;
    u16 usages[32]; int nusg = 0;
    dev->parsed = 0; dev->tip_off = -1; dev->x_off = dev->y_off = -1;
    dev->is_kbd = 0; dev->kbd_report_id = 0;
    while (i < len) {
        u8 p = rd[i++];
        int sz = p & 3, type = (p >> 2) & 3, tag = (p >> 4) & 0xF, k;
        u32 data = 0;
        if (sz == 3) sz = 4;
        for (k = 0; k < sz && i < len; k++) data |= (u32)rd[i++] << (8 * k);
        if (type == 1) {                                  /* global */
            switch (tag) {
            case 0x0: page = (int)data; break;            /* Usage Page */
            case 0x2: lmax = (int)data; break;            /* Logical Maximum */
            case 0x7: rsize = (int)data; break;           /* Report Size */
            case 0x8: cur_id = (int)data; bitpos = 0; break;  /* Report ID */
            case 0x9: rcount = (int)data; break;          /* Report Count */
            }
        } else if (type == 2) {                           /* local */
            if (tag == 0x0 && nusg < 32) usages[nusg++] = (u16)data;   /* Usage */
        } else if (type == 0) {                           /* main */
            if (tag == 0x8) {                             /* Input */
                int f;
                if (page == 0x07 && !dev->is_kbd) {       /* Keyboard/Keypad */
                    dev->is_kbd = 1; dev->kbd_report_id = (u8)cur_id;
                }
                for (f = 0; f < rcount; f++) {
                    u16 u = (f < nusg) ? usages[f] : (nusg ? usages[nusg - 1] : 0);
                    int off = bitpos + f * rsize;
                    if (page == 0x01 && u == 0x30 && dev->x_off < 0) {
                        dev->x_off = off; dev->x_bits = rsize; dev->x_lmax = lmax; dev->report_id = (u8)cur_id;
                    } else if (page == 0x01 && u == 0x31 && dev->y_off < 0) {
                        dev->y_off = off; dev->y_bits = rsize; dev->y_lmax = lmax;
                    } else if (((page == 0x0D && u == 0x42) ||
                                (page == 0x09 && u == 0x01)) && dev->tip_off < 0) {
                        dev->tip_off = off;
                    }
                }
                bitpos += rsize * rcount;
            }
            nusg = 0;                                     /* usages consumed */
        }
    }
    if (dev->x_off >= 0 && dev->y_off >= 0) dev->parsed = 1;
}

static void parse_report_desc(hiddev *dev)
{
    static u8 rd[512];
    int len = dev->rd_len, n;
    if (len <= 0) return;
    if (len > (int)sizeof rd) len = sizeof rd;
    n = dw_reg_read(dev->addr, dev->rd_reg, rd, len);
    if (n >= 4) parse_rd(dev, rd, n);
}

static int g_nbars, g_nctrl;       /* diagnostics (uno_i2c_hid_status) */

int uno_i2c_hid_init(void)
{
    u64 bars[8];
    int nb, bi, ai, di;
    nb = list_bars(bars, 8);
    g_nbars = nb;
    for (bi = 0; bi < nb; bi++) {
        /* NB: cast straight through uintptr_t (pointer-width, 64-bit on this
         * LLP64 mingw target) - an intermediate (unsigned long) is only 32 bits
         * here and TRUNCATES an above-4GB BAR, which is exactly where the
         * Surface's LPSS I2C controllers live (same 64-bit-BAR wall the SAM
         * battery path hit). Truncation -> wrong MMIO -> COMP_TYPE miss -> the
         * "N bars / 0 ctrl" symptom. */
        g_i2c = (volatile u8 *)(uintptr_t)bars[bi];
        if (!g_i2c) continue;
        /* confirm this BAR really is a DesignWare I2C before poking it - an
         * unmapped/foreign BAR reads 0xFFFFFFFF here, so we skip it and stay
         * inert (e.g. under QEMU, which has no LPSS I2C). */
        if (r32(DW_IC_COMP_TYPE) != DW_COMP_TYPE_VALUE) continue;
        g_nctrl++;
        /* Intel LPSS leaves the DW core in reset until its private RESETS
         * register is released; a controller still in reset never drives SCL,
         * so every transfer times out (the "found controllers, no device"
         * symptom). Assert then release host controller + iDMA, and settle. */
        w32(LPSS_RESETS, 0x0);
        { int t = 20000; while (t--) spin(2); }
        w32(LPSS_RESETS, 0x7);
        { int t = 100000; while (t--) spin(2); }
        /* probe every slave x descriptor-register; classify each valid device
         * into the pointer or keyboard slot. Keep scanning this + later
         * controllers until BOTH slots are filled (the Surface has them on the
         * same controller at different addresses). */
        for (ai = 0; ai < (int)sizeof kAddrs; ai++)
            for (di = 0; di < (int)(sizeof kDescRegs / sizeof kDescRegs[0]); di++) {
                hiddev tmp;
                if (g_ptr.present && g_kbd.present) return 1;
                if (!try_hid(&tmp, kAddrs[ai], kDescRegs[di])) continue;
                hid_power_reset(&tmp);
                parse_report_desc(&tmp);
                if (tmp.is_kbd && !g_kbd.present) {
                    g_kbd = tmp; g_kbd.present = 1; hid_kbd_reset(&g_kbd.kbd);
                } else if (tmp.parsed && !g_ptr.present) {
                    g_ptr = tmp; g_ptr.present = 1;
                } else if (!g_ptr.present && !tmp.is_kbd) {
                    /* an unparsed pointer-ish device: keep as the pointer
                     * fallback (the old single-device behaviour) */
                    g_ptr = tmp; g_ptr.present = 1;
                }
            }
    }
    return (g_ptr.present || g_kbd.present) ? 1 : 0;
}

/* Extract nbits starting at bitoff from d[], which holds nbytes valid bytes.
 * Every field (bit offset, width) comes from the device's HID report
 * descriptor, so both are untrusted: clamp nbits to a word and stop at the end
 * of the buffer rather than reading past it. */
static int getbits(const u8 *d, int nbytes, int bitoff, int nbits)
{
    int v = 0, i;
    if (bitoff < 0 || nbits <= 0) return 0;
    if (nbits > 31) nbits = 31;
    for (i = 0; i < nbits; i++) {
        int b = bitoff + i;
        if ((b >> 3) >= nbytes) break;
        if (d[b >> 3] & (1 << (b & 7))) v |= (1 << i);
    }
    return v;
}

/* ---- input reports ------------------------------------------------------ *
 * Report on the wire is [len16][reportID][data...]. With a parsed descriptor we
 * pull X/Y/tip from their real bit offsets and scale to 0..32767; otherwise we
 * fall back to the common WPT first-finger shape (refine via _dump). */
int uno_i2c_hid_poll(int *absx, int *absy, int *buttons)
{
    u8 rep[64];
    int n, len;
    if (!g_ptr.present) return 0;
    g_i2c = g_ptr.i2c;                            /* select its controller */
    n = dw_reg_read(g_ptr.addr, g_ptr.input_reg, rep, g_ptr.max_input);
    if (n < 2) return 0;
    len = rep[0] | (rep[1] << 8);
    if (len < 2 || len > n) return 0;
    g_report_len = (len < (int)sizeof g_report) ? len : (int)sizeof g_report;
    { int i; for (i = 0; i < g_report_len; i++) g_report[i] = rep[i]; }

    if (g_ptr.parsed) {
        int base = g_ptr.report_id ? 3 : 2;      /* skip len16 [+ reportID] */
        const u8 *data = rep + base;
        int avail = n - base;                     /* valid bytes behind `data` */
        int x, y, tip;
        if (g_ptr.report_id && rep[2] != g_ptr.report_id) return 0;  /* other report */
        if (len <= base || avail <= 0) return 0;
        x = getbits(data, avail, g_ptr.x_off, g_ptr.x_bits);
        y = getbits(data, avail, g_ptr.y_off, g_ptr.y_bits);
        tip = (g_ptr.tip_off >= 0) ? getbits(data, avail, g_ptr.tip_off, 1) : 0;
        if (g_ptr.x_lmax > 0) x = (int)((long)x * 32767 / g_ptr.x_lmax);
        if (g_ptr.y_lmax > 0) y = (int)((long)y * 32767 / g_ptr.y_lmax);
        *absx = x < 0 ? 0 : (x > 32767 ? 32767 : x);
        *absy = y < 0 ? 0 : (y > 32767 ? 32767 : y);
        *buttons = tip ? 1 : 0;
        return 1;
    }
    if (len >= 8) {                              /* unparsed fallback */
        int btn = rep[3] & 0x07;
        int x = rep[4] | (rep[5] << 8);
        int y = rep[6] | (rep[7] << 8);
        *absx = (x > 32767) ? 32767 : (x < 0 ? 0 : x);
        *absy = (y > 32767) ? 32767 : (y < 0 ? 0 : y);
        *buttons = btn ? 1 : 0;
        return 1;
    }
    return 0;
}

/* poll the keyboard device: read its input report, hand the 8-byte boot-shape
 * payload (mod, reserved, 6 keycodes) to the shared hid_kbd edge translator. */
int uno_i2c_hid_kbd_poll(uno_i2c_key_fn emit, void *ctx)
{
    u8 rep[64];
    int n, len, base;
    if (!g_kbd.present) return 0;
    g_i2c = g_kbd.i2c;
    n = dw_reg_read(g_kbd.addr, g_kbd.input_reg, rep, g_kbd.max_input);
    if (n < 2) return 1;                         /* present but no report now */
    len = rep[0] | (rep[1] << 8);
    if (len < 2 || len > n) return 1;
    base = 2 + (g_kbd.kbd_report_id ? 1 : 0);    /* skip len16 [+ reportID] */
    if (g_kbd.kbd_report_id && rep[2] != g_kbd.kbd_report_id) return 1;  /* other report */
    if (n - base >= 8)
        hid_kbd_report(&g_kbd.kbd, rep + base, (hid_key_fn)emit, ctx);
    return 1;
}

int uno_i2c_hid_dump(unsigned char *out, int max)
{
    int i, n = g_report_len < max ? g_report_len : max;
    for (i = 0; i < n; i++) out[i] = g_report[i];
    return n;
}

int uno_i2c_hid_present(void)     { return g_ptr.present; }
int uno_i2c_hid_kbd_present(void) { return g_kbd.present; }

void uno_i2c_hid_status(int *nbars, int *nctrl, int *present, int *addr, int *parsed)
{
    if (nbars)   *nbars   = g_nbars;
    if (nctrl)   *nctrl   = g_nctrl;
    if (present) *present = g_ptr.present;
    if (addr)    *addr    = g_ptr.addr;
    if (parsed)  *parsed  = g_ptr.parsed;
}

void uno_i2c_hid_diag(int *saw_ack, unsigned *abrt)
{
    if (saw_ack) *saw_ack = g_saw_bytes;
    if (abrt)    *abrt    = (unsigned)g_last_abrt;
}

#endif /* UNO_I2C_TRACKPAD */
