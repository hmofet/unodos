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
 * CONFIG - fill these from Linux on the SAME machine, then rebuild with
 * -DUNO_I2C_TRACKPAD. (ACPI/AML parsing to discover them automatically is a
 * future step.)
 *
 *   I2C controller MMIO base:  the LPSS I2C the pad is on. If it is PCI-
 *     visible we auto-find it (see discover_controller); otherwise set
 *     I2C_HID_BASE to the address from Linux, e.g. from
 *        dmesg | grep i2c_designware      (look for the MMIO region), or
 *        the platform device's resource under sysfs.
 *   Slave address:  i2cdetect -l   then i2cdetect -y <bus>   (Synaptics is
 *     often 0x2c, Elan 0x15/0x2a).
 *   HID descriptor register:  the _DSM HIDREG, almost always 0x0020 (some
 *     0x0001). dmesg 'i2c_hid' / the ACPI I2cSerialBus descriptor.
 * ======================================================================== */
#ifndef I2C_HID_BASE
#define I2C_HID_BASE   0            /* 0 = auto-find a PCI-visible LPSS I2C */
#endif
#ifndef I2C_HID_ADDR
#define I2C_HID_ADDR   0x2c         /* Synaptics default */
#endif
#ifndef I2C_HID_DESCREG
#define I2C_HID_DESCREG 0x0020
#endif

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

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

static volatile u8 *g_i2c;         /* controller MMIO base */
static int g_present;
static u8  g_report[64];
static int g_report_len;

/* HID-over-I2C descriptor fields we use */
static u16 g_input_reg, g_max_input, g_cmd_reg;

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

/* ---- discovery: a PCI-visible Intel LPSS I2C controller ----------------- */
static int discover_controller(void)
{
#if I2C_HID_BASE
    g_i2c = (volatile u8 *)(unsigned long)(uintptr_t)I2C_HID_BASE;
    return 1;
#else
    pci_dev d;
    /* Intel (8086) serial-bus / other (class 0x0C, subclass 0x80) = LPSS I2C
       in PCI mode. In ACPI mode it is hidden - then set I2C_HID_BASE manually. */
    if (pci_find_class(0x0C, 0x80, &d) && d.vendor == 0x8086) {
        pci_enable_bus_master(&d);
        g_i2c = (volatile u8 *)(unsigned long)(uintptr_t)pci_bar(&d, 0);
        return g_i2c != 0;
    }
    return 0;
#endif
}

/* ---- HID-over-I2C bring-up ---------------------------------------------- */
static int hid_bringup(void)
{
    u8 desc[30];
    int n = dw_reg_read(I2C_HID_ADDR, I2C_HID_DESCREG, desc, sizeof desc);
    if (n < 30) return 0;
    /* HID descriptor: wHIDDescLength@0 must be 30; input register @8; max
       input length @10; command register @16 */
    if (desc[0] != 30) return 0;
    g_input_reg = desc[8] | (desc[9] << 8);
    g_max_input = desc[10] | (desc[11] << 8);
    g_cmd_reg   = desc[16] | (desc[17] << 8);
    if (g_max_input == 0 || g_max_input > sizeof g_report) g_max_input = sizeof g_report;

    /* SET_POWER on: [cmdReg LE][0x00, 0x08] */
    { u8 c[4] = { g_cmd_reg & 0xFF, g_cmd_reg >> 8, 0x00, 0x08 };
      dw_write(I2C_HID_ADDR, c, 4); }
    /* RESET: [cmdReg LE][0x00, 0x01] */
    { u8 c[4] = { g_cmd_reg & 0xFF, g_cmd_reg >> 8, 0x00, 0x01 };
      dw_write(I2C_HID_ADDR, c, 4); }
    { int t = 500000; while (t--) spin(4); }    /* let reset settle */
    return 1;
}

int uno_i2c_hid_init(void)
{
    if (!discover_controller()) return 0;
    g_present = hid_bringup();
    return g_present;
}

/* ---- input reports ------------------------------------------------------ *
 * Read maxInputLength bytes; report layout is [len LE][reportID][data...].
 * The data layout is device-specific (defined by the HID report descriptor,
 * which we don't parse yet). This first pass handles the common HID BOOT-mouse
 * and Windows-Precision-Touchpad first-finger shapes; use uno_i2c_hid_dump()
 * to capture the real bytes and refine here. */
int uno_i2c_hid_poll(int *absx, int *absy, int *buttons)
{
    u8 rep[64];
    int n, len;
    if (!g_present) return 0;
    n = dw_reg_read(I2C_HID_ADDR, g_input_reg, rep, g_max_input);
    if (n < 2) return 0;
    len = rep[0] | (rep[1] << 8);
    if (len < 2 || len > n) return 0;
    /* cache raw for dumping */
    g_report_len = (len < (int)sizeof g_report) ? len : (int)sizeof g_report;
    { int i; for (i = 0; i < g_report_len; i++) g_report[i] = rep[i]; }

    /* Heuristic first-pass parse (WILL be refined from real dumps):
     * treat rep[2] as report id; for a WPT contact the first finger is at a
     * fixed offset with a 16-bit X/Y and a tip/button byte. We expose the
     * first plausible 16-bit X/Y pair >byte 3 and any low-bit button. */
    if (len >= 8) {
        int btn = rep[3] & 0x07;                 /* tip/switch bits (guess) */
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
