/* cosmo64/i2c.c -- polled MTK I2C for bus 4 (the AW9523 keyboard's), MT6771.
 *
 * Facts extracted from the vendor kernel (i2c-mtk.c/.h v2, clk-mt6771.c,
 * pinctrl-mtk-mt6771.h, k71v1_64_bsp.dws/.dts -- see the 2026-09-01 fact
 * sheet in the git log). The essentials:
 *
 *  - Controller at 0x11008000; this is an ARBITRATED bus, so every transfer
 *    register lives in the +0x100 channel bank; the +0 shadow bank is used
 *    only by init/recovery. 16-bit registers.
 *  - PIO/FIFO transfers up to 8 bytes need no DMA. A register write is one
 *    2-byte transaction; a register read is WRRD (CONTROL.DIR_CHANGE|RS +
 *    TRANSAC_LEN=2): write the reg address, repeated-start, read N.
 *  - Clock gates (write BIT to the CLR address to ungate): I2C1 bit12 @
 *    0x10001084, AP_DMA bit18 @ 0x1000108C, I2C1_ARBITER bit21 @ 0x100010A8.
 *  - Bus speed: 100 kHz (no clock-frequency in the DT; hs_only is inert at
 *    that speed). Timing depends on the i2c_sel mux at 0x100000C0[1:0],
 *    inherited from the preloader -- read it and pick the matching pair.
 *  - Pads: SCL4/SDA4 = GPIO105/106 mode 1 (RMW 0x100053D0), internal pulls
 *    on by boot default. AW9523 SHDN/HWEN = GPIO175, boot-default output-LOW
 *    (chip held in shutdown until raised).
 */

#include "cosmo64.h"

#define R16(a) (*(volatile unsigned short *)(c64_u64)(a))
#define R32(a) (*(volatile c64_u32 *)(c64_u64)(a))

#define I2C_BASE 0x11008000u
#define CH 0x100u                        /* the arbitrated channel bank */
#define APDMA 0x11000100u

/* v2 register offsets */
#define O_DATA 0x00
#define O_SLAVE 0x04
#define O_INTR_MASK 0x08
#define O_INTR_STAT 0x0C
#define O_CONTROL 0x10
#define O_XFER_LEN 0x14
#define O_TRANSAC_LEN 0x18
#define O_DELAY_LEN 0x1C
#define O_TIMING 0x20
#define O_START 0x24
#define O_EXT_CONF 0x28
#define O_LTIMING 0x2C
#define O_HS 0x30
#define O_IO_CONFIG 0x34
#define O_FIFO_ADDR_CLR 0x38
#define O_MCU_INTR 0x40
#define O_XFER_LEN_AUX 0x44
#define O_CLOCK_DIV 0x48
#define O_HW_TIMEOUT 0x4C
#define O_SOFTRESET 0x50

#define ST_COMP 0x0001
#define ST_ACKERR 0x0002
#define ST_HS_NACK 0x0004
#define ST_ARB_LOST 0x0008
#define ST_TIMEOUT 0x0020
#define ST_MAS_ERR 0x0100
#define ST_ERRS (ST_ACKERR | ST_HS_NACK | ST_ARB_LOST | ST_TIMEOUT | ST_MAS_ERR)

static unsigned short g_timing = 0x0217, g_ltiming = 0x0096;  /* 68.25 MHz src */
static int g_ready;

static void spin_us(c64_u32 us)
{
    c64_u64 until = c64_cnt_now() + (c64_u64)us * 13 + 13;    /* 13 MHz */
    while (c64_cnt_now() < until)
        ;
}

void c64_kbd_power(int on)
{
    R32(0x10005450) &= ~(7u << 28);      /* GPIO175 mode = GPIO */
    R32(0x10005054) = 1u << 15;          /* DIR_SET: output */
    if (on)
        R32(0x10005154) = 1u << 15;      /* DOUT_SET */
    else
        R32(0x10005158) = 1u << 15;      /* DOUT_CLR */
    __asm__ volatile("dsb sy" ::: "memory");
}

static void init_hw(void)
{
    /* the shadow bank (+0), per mt_i2c_init_hw */
    R16(I2C_BASE + O_INTR_MASK) = 0;
    R16(I2C_BASE + O_INTR_STAT) = R16(I2C_BASE + O_INTR_STAT);
    R16(I2C_BASE + O_SOFTRESET) = 1;
    R16(I2C_BASE + O_IO_CONFIG) = 0x0003;        /* open-drain */
    R16(I2C_BASE + O_TIMING) = g_timing;
    R16(I2C_BASE + O_LTIMING) = g_ltiming;
    R16(I2C_BASE + O_HS) = 0;
    /* APDMA warm reset (the driver does this even for PIO) */
    R32(APDMA + 0x0C) = 1;
    spin_us(10);
}

int c64_i2c_init(void)
{
    if (g_ready)
        return 0;

    /* ungate main + dma + arbiter (CLR registers) */
    R32(0x10001084) = 1u << 12;
    R32(0x1000108C) = 1u << 18;
    R32(0x100010A8) = 1u << 21;
    __asm__ volatile("dsb sy" ::: "memory");

    /* pads: SCL4/SDA4 = GPIO105/106 mode 1; make sure the pulls are up */
    c64_u32 m = R32(0x100053D0);
    m = (m & ~((7u << 4) | (7u << 8))) | (1u << 4) | (1u << 8);
    R32(0x100053D0) = m;
    R32(0x11D20060) |= (1u << 30) | (1u << 31);  /* PULLEN */
    R32(0x11D20080) |= (1u << 30) | (1u << 31);  /* PULLSEL = up */

    /* timing by the inherited i2c_sel mux (0=26M, 1=68.25M, 2=124.8M source,
     * then /5 by CLOCK_DIV) -- values from the vendor speed math @ 100 kHz */
    switch (R32(0x100000C0) & 3) {
    case 0: g_timing = 0x0019 | 1; g_ltiming = 0x0019; break;
    case 2: g_timing = 0x0419 | 1; g_ltiming = 0x0118; break;
    default: g_timing = 0x0217; g_ltiming = 0x0096; break;
    }

    init_hw();
    g_ready = 1;
    return 0;
}

/* one PIO transaction on the channel bank. wr: bytes to send (starts with the
 * register address); rd: NULL for a plain write, else nrd bytes via WRRD. */
static int xfer(c64_u8 dev, const c64_u8 *wr, int nwr, c64_u8 *rd, int nrd)
{
    c64_u32 B = I2C_BASE + CH;
    R16(B + O_CLOCK_DIV) = 0x0404;
    R16(B + O_CONTROL) = rd ? 0x003A : 0x0028;   /* ACKERR_DET|CLK_EXT
                                                    (+DIR_CHANGE|RS for WRRD) */
    R16(B + O_EXT_CONF) = 0x8001;
    if (!rd)
        R16(B + O_DELAY_LEN) = 0x0002;
    R16(B + O_IO_CONFIG) = 0x0003;
    R16(B + O_TIMING) = g_timing;
    R16(B + O_LTIMING) = g_ltiming;
    R16(B + O_HS) = 0;
    R16(B + O_HW_TIMEOUT) = 0x018C;
    R16(B + O_SLAVE) = (unsigned short)(dev << 1);
    R16(B + O_INTR_STAT) = 0x01FF;
    R16(B + O_FIFO_ADDR_CLR) = 0x0005;           /* MCH variant: ch_offset!=0 */
    R16(B + O_INTR_MASK) = 0;                    /* polled: no irq line */
    if (rd) {
        R16(B + O_XFER_LEN) = (unsigned short)nwr;
        R16(B + O_XFER_LEN_AUX) = (unsigned short)nrd;
        R16(B + O_TRANSAC_LEN) = 2;
    } else {
        R16(B + O_XFER_LEN) = (unsigned short)nwr;
        R16(B + O_TRANSAC_LEN) = 1;
    }
    for (int i = 0; i < nwr; i++)
        R16(B + O_DATA) = wr[i];
    R16(B + O_MCU_INTR) = 1;                     /* v2 requirement */
    __asm__ volatile("dsb sy" ::: "memory");
    R16(B + O_START) = 1;

    unsigned short st = 0;
    c64_u64 until = c64_cnt_now() + 13 * 20000;  /* 20 ms cap */
    for (;;) {
        st = R16(B + O_INTR_STAT);
        if (st & (ST_COMP | ST_ERRS))
            break;
        if (c64_cnt_now() > until) {
            st = 0;
            break;
        }
    }
    R16(B + O_INTR_STAT) = st;                   /* write-1-clear */
    if (!(st & ST_COMP) || (st & ST_ERRS)) {
        R16(B + O_FIFO_ADDR_CLR) = 0x0005;
        init_hw();
        R16(I2C_BASE + O_START) = 0x0002;        /* shadow: resume arbitration */
        return -1;
    }
    for (int i = 0; i < nrd; i++)
        rd[i] = (c64_u8)R16(B + O_DATA);
    return 0;
}

int c64_i2c_write_reg(c64_u8 dev, c64_u8 reg, c64_u8 val)
{
    c64_u8 wr[2] = { reg, val };
    return xfer(dev, wr, 2, 0, 0);
}

int c64_i2c_read_reg(c64_u8 dev, c64_u8 reg)
{
    c64_u8 rd = 0;
    if (xfer(dev, &reg, 1, &rd, 1) < 0)
        return -1;
    return rd;
}
