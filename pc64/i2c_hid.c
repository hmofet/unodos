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
int uno_i2c_hid_dump(unsigned char *o, int m) { (void)o;(void)m; return 0; }
int uno_i2c_hid_present(void) { return 0; }

#else  /* ================= UNO_I2C_TRACKPAD enabled ==================== */

#include "pc64_pci.h"
#include <stdint.h>

/* ===========================================================================
 * SELF-CONFIGURING bring-up. Rather than depend on ACPI/AML constants, we
 * enumerate the machine: scan every PCI-visible Intel LPSS I2C controller, and
 * on each probe the handful of slave-address x HID-descriptor-register
 * combinations real trackpads use, validating each by the HID descriptor's
 * signature (wHIDDescLength==30, bcdVersion==0x0100). Whatever answers with a
 * valid descriptor IS the trackpad - no hand-supplied constants. An override
 * (-DI2C_HID_BASE=... etc.) is still honoured for a controller hidden from PCI
 * (ACPI-only mode), the one case enumeration can't reach.
 * ======================================================================== */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

/* candidates a Windows-Precision-Touchpad / HID-over-I2C pad answers on */
static const u8  kAddrs[]   = { 0x2c, 0x15, 0x2a, 0x10, 0x20 };
static const u16 kDescRegs[] = { 0x0020, 0x0001 };

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

static volatile u8 *g_i2c;         /* controller MMIO base (the found one) */
static u8  g_addr;                  /* the found slave address */
static int g_present;
static u8  g_report[64];
static int g_report_len;

/* HID-over-I2C descriptor fields we use */
static u16 g_input_reg, g_max_input, g_cmd_reg, g_rd_reg, g_rd_len;

/* report-descriptor parse results: bit offsets (within the report data, after
 * the leading report-id byte) + sizes + logical maxima for scaling. When
 * g_parsed is 0 the poll falls back to the common WPT/boot-mouse shape. */
static int g_parsed;
static u8  g_report_id;
static int g_x_off, g_x_bits, g_x_lmax;
static int g_y_off, g_y_bits, g_y_lmax;
static int g_tip_off;              /* tip-switch / button-1 bit, -1 if none */

static u32 r32(u32 o) { return *(volatile u32 *)(g_i2c + o); }
static void w32(u32 o, u32 v) { *(volatile u32 *)(g_i2c + o) = v; }
static void spin(volatile int n) { while (n-- > 0) __asm__ volatile (""); }

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
    if (r32(DW_IC_TX_ABRT_SOURCE)) { (void)r32(DW_IC_CLR_TX_ABRT); return -1; }
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
 * map on success. Validation by signature = self-configuring probe. */
static int try_hid(u8 addr, u16 descReg)
{
    u8 d[30];
    int n = dw_reg_read(addr, descReg, d, sizeof d);
    if (n < 30) return 0;
    if ((d[0] | (d[1] << 8)) != 30)      return 0;   /* wHIDDescLength */
    if ((d[2] | (d[3] << 8)) != 0x0100)  return 0;   /* bcdVersion 1.00 */
    g_addr      = addr;
    g_rd_len    = (u16)(d[4]  | (d[5]  << 8));
    g_rd_reg    = (u16)(d[6]  | (d[7]  << 8));
    g_input_reg = (u16)(d[8]  | (d[9]  << 8));
    g_max_input = (u16)(d[10] | (d[11] << 8));
    g_cmd_reg   = (u16)(d[16] | (d[17] << 8));
    if (g_max_input == 0 || g_max_input > sizeof g_report) g_max_input = sizeof g_report;
    return 1;
}

static void hid_power_reset(void)
{
    u8 c[4];
    c[0] = g_cmd_reg & 0xFF; c[1] = g_cmd_reg >> 8; c[2] = 0x00; c[3] = 0x08;  /* power on */
    dw_write(g_addr, c, 4);
    c[2] = 0x00; c[3] = 0x01;                                                  /* reset */
    dw_write(g_addr, c, 4);
    { int t = 500000; while (t--) spin(4); }         /* let reset settle */
}

/* ---- HID report-descriptor parser: find the X / Y / tip fields ---------- *
 * Walk the item stream tracking usage page, report size/count and the running
 * bit offset, and record the bit position of Generic-Desktop X (page 1 usage
 * 0x30), Y (0x31) and the tip switch (Digitizer page 0x0D usage 0x42, or
 * Button-1). This replaces the fixed-offset guess with the device's own map. */
static void parse_rd(const u8 *rd, int len)
{
    int i = 0, page = 0, rsize = 0, rcount = 0, bitpos = 0, lmax = 0, cur_id = 0;
    u16 usages[32]; int nusg = 0;
    g_parsed = 0; g_tip_off = -1; g_x_off = g_y_off = -1;
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
                for (f = 0; f < rcount; f++) {
                    u16 u = (f < nusg) ? usages[f] : (nusg ? usages[nusg - 1] : 0);
                    int off = bitpos + f * rsize;
                    if (page == 0x01 && u == 0x30 && g_x_off < 0) {
                        g_x_off = off; g_x_bits = rsize; g_x_lmax = lmax; g_report_id = (u8)cur_id;
                    } else if (page == 0x01 && u == 0x31 && g_y_off < 0) {
                        g_y_off = off; g_y_bits = rsize; g_y_lmax = lmax;
                    } else if (((page == 0x0D && u == 0x42) ||
                                (page == 0x09 && u == 0x01)) && g_tip_off < 0) {
                        g_tip_off = off;
                    }
                }
                bitpos += rsize * rcount;
            }
            nusg = 0;                                     /* usages consumed */
        }
    }
    if (g_x_off >= 0 && g_y_off >= 0) g_parsed = 1;
}

static void parse_report_desc(void)
{
    static u8 rd[512];
    int len = g_rd_len, n;
    if (len <= 0) return;
    if (len > (int)sizeof rd) len = sizeof rd;
    n = dw_reg_read(g_addr, g_rd_reg, rd, len);
    if (n >= 4) parse_rd(rd, n);
}

int uno_i2c_hid_init(void)
{
    u64 bars[8];
    int nb, bi, ai, di;
    nb = list_bars(bars, 8);
    for (bi = 0; bi < nb; bi++) {
        g_i2c = (volatile u8 *)(unsigned long)(uintptr_t)bars[bi];
        if (!g_i2c) continue;
        /* confirm this BAR really is a DesignWare I2C before poking it - an
         * unmapped/foreign BAR reads 0xFFFFFFFF here, so we skip it and stay
         * inert (e.g. under QEMU, which has no LPSS I2C). */
        if (r32(DW_IC_COMP_TYPE) != DW_COMP_TYPE_VALUE) continue;
        for (ai = 0; ai < (int)sizeof kAddrs; ai++)
            for (di = 0; di < (int)(sizeof kDescRegs / sizeof kDescRegs[0]); di++)
                if (try_hid(kAddrs[ai], kDescRegs[di])) {
                    hid_power_reset();
                    parse_report_desc();
                    g_present = 1;
                    return 1;
                }
    }
    return 0;
}

static int getbits(const u8 *d, int bitoff, int nbits)
{
    int v = 0, i;
    for (i = 0; i < nbits; i++) {
        int b = bitoff + i;
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
    if (!g_present) return 0;
    n = dw_reg_read(g_addr, g_input_reg, rep, g_max_input);
    if (n < 2) return 0;
    len = rep[0] | (rep[1] << 8);
    if (len < 2 || len > n) return 0;
    g_report_len = (len < (int)sizeof g_report) ? len : (int)sizeof g_report;
    { int i; for (i = 0; i < g_report_len; i++) g_report[i] = rep[i]; }

    if (g_parsed) {
        int base = g_report_id ? 3 : 2;          /* skip len16 [+ reportID] */
        const u8 *data = rep + base;
        int x, y, tip;
        if (g_report_id && rep[2] != g_report_id) return 0;   /* a different report */
        if (len <= base) return 0;
        x = getbits(data, g_x_off, g_x_bits);
        y = getbits(data, g_y_off, g_y_bits);
        tip = (g_tip_off >= 0) ? getbits(data, g_tip_off, 1) : 0;
        if (g_x_lmax > 0) x = (int)((long)x * 32767 / g_x_lmax);
        if (g_y_lmax > 0) y = (int)((long)y * 32767 / g_y_lmax);
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

int uno_i2c_hid_dump(unsigned char *out, int max)
{
    int i, n = g_report_len < max ? g_report_len : max;
    for (i = 0; i < n; i++) out[i] = g_report[i];
    return n;
}

int uno_i2c_hid_present(void) { return g_present; }

#endif /* UNO_I2C_TRACKPAD */
