/* cosmo64/i2c.c -- polled MTK I2C for the MT6771, buses 4 (AW9523 keyboard)
 * and 0 (NT36672 touch panel).
 *
 * Facts extracted from the vendor kernel (i2c-mtk.c/.h v2, clk-mt6771.c,
 * pinctrl-mtk-mt6771.h, the DWS/DTS) -- see the git log. The essentials:
 *
 *  - v2 register file, 16-bit registers. Bus 4 is ARBITRATED, so its transfer
 *    registers live in a +0x100 channel bank (the +0 shadow bank is used only
 *    by init/recovery) and its FIFO clear takes the MCH bit; bus 0 has no
 *    channel offset and uses the plain forms. That per-bus difference is the
 *    whole reason this file is table-driven.
 *  - PIO/FIFO only: the FIFO is 8 bytes, and the vendor driver switches to
 *    DMA past that, so every transfer here stays <= 8 bytes each way. (The
 *    touch driver reads one finger, not all ten, precisely to stay inside it.)
 *  - A register read is WRRD: CONTROL.DIR_CHANGE|RS with TRANSAC_LEN=2 --
 *    write the offset, repeated-start, read N.
 *  - Bus speed: 400 kHz (Fast mode), computed at runtime from the i2c_sel mux
 *    the preloader left at 0x100000C0[1:0] rather than assumed. It used to be
 *    100 kHz, which is the vendor driver's default for a client that asks for
 *    nothing -- but BOTH parts on this machine are Fast-mode devices (the
 *    AW9523 expander and the NT36672 touch controller), and the shell polls
 *    them every frame with no interrupt to lean on, so the bus rate is
 *    straightforwardly a quarter of the per-loop input cost. A driver whose
 *    probe finds nothing at 400 kHz calls c64_i2c_set_khz(bus, 100) and tries
 *    again before declaring its part absent, so a part that turns out not to
 *    like Fast mode degrades to the old behaviour instead of disappearing.
 */

#include "cosmo64.h"

#define R16(a) (*(volatile unsigned short *)(c64_u64)(a))
#define R32(a) (*(volatile c64_u32 *)(c64_u64)(a))

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
#define ST_ERRS 0x012E                   /* ACKERR|HS_NACK|ARB_LOST|TIMEOUT|MAS_ERR */

struct bus {
    c64_u32 base, ch, apdma;
    c64_u32 gate_reg, gate_bit;          /* infracfg CLR register + bit */
    c64_u32 arb_reg, arb_bit;            /* the arbiter gate, 0 if none */
    c64_u32 mux_reg, mux_clr, mux_set;   /* pinmux RMW for SCL/SDA */
    int ready;
    int khz;                             /* 0 until set; the programmed rate */
    unsigned short timing, ltiming;
};

static struct bus g_bus[2] = {
    /* C64_I2C_KBD: bus 4 @ 0x11008000, arbitrated (+0x100), SCL4/SDA4 =
     * GPIO105/106 mode 1; gates I2C1 bit12 + I2C1_ARBITER bit21 */
    { 0x11008000, 0x100, 0x11000100, 0x10001084, 12, 0x100010A8, 21,
      0x100053D0, (7u << 4) | (7u << 8), (1u << 4) | (1u << 8), 0, 0, 0, 0 },
    /* C64_I2C_TP: bus 0 @ 0x11007000, no channel offset, SDA0/SCL0 =
     * GPIO82/83 mode 1; gate I2C0 bit11, no arbiter */
    { 0x11007000, 0x000, 0x11000080, 0x10001084, 11, 0, 0,
      0x100053A0, (7u << 8) | (7u << 12), (1u << 8) | (1u << 12), 0, 0, 0, 0 },
};

/* The bus clock, as the mux leaves it: the selected source divided by the /5
 * in CLOCK_DIV. Read once, because both buses share the mux. */
static c64_u32 g_bus_clk = 13650000u;            /* 68.25 MHz / 5 */
static int g_clk_done;

static void init_hw(struct bus *b);

static void spin_us(c64_u32 us)
{
    c64_u64 until = c64_cnt_now() + (c64_u64)us * 13 + 13;    /* 13 MHz */
    while (c64_cnt_now() < until)
        ;
}

/* THE TIMING RECIPE, which is the vendor driver's and not a guess. The bus
 * runs at clk / (2 * sample_cnt * step_cnt); the register carries each count
 * MINUS ONE, with sample in bits 8..10 of TIMING (bits 6..8 of LTIMING) and
 * step in the low six bits of both. Walk sample_cnt upward and take the first
 * step_cnt that fits its six-bit field -- which for every rate this machine
 * uses lands on sample_cnt 1, the shortest and most accurate arrangement.
 *
 * The result is always AT OR BELOW the rate asked for, because step_cnt is
 * rounded up: overshooting an I2C bus is how a part that works becomes a part
 * that intermittently NAKs. */
int c64_i2c_set_khz(int bus, int khz)
{
    if (bus < 0 || bus > 1 || khz <= 0)
        return -1;
    struct bus *b = &g_bus[bus];
    c64_u32 target = (c64_u32)khz * 1000u;
    c64_u32 opt = (g_bus_clk / 2u + target - 1u) / target;   /* sample*step */
    c64_u32 sample, step = 0;

    for (sample = 1; sample <= 8; sample++) {
        step = (opt + sample - 1u) / sample;
        if (step <= 64u)
            break;
    }
    if (sample > 8) {                    /* slower than the divider can go */
        sample = 8;
        step = 64;
    }
    b->timing = (unsigned short)(((sample - 1u) << 8) | (step - 1u));
    b->ltiming = (unsigned short)(((sample - 1u) << 6) | (step - 1u));
    b->khz = (int)(g_bus_clk / (2u * sample * step) / 1000u);
    if (b->ready)
        init_hw(b);                      /* take effect on the live bus */
    return 0;
}

int c64_i2c_khz(int bus)
{
    return (bus < 0 || bus > 1) ? 0 : g_bus[bus].khz;
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

static void init_hw(struct bus *b)
{
    c64_u32 B = b->base;                 /* the shadow bank, per init_hw */
    R16(B + O_INTR_MASK) = 0;
    R16(B + O_INTR_STAT) = R16(B + O_INTR_STAT);
    R16(B + O_SOFTRESET) = 1;
    R16(B + O_IO_CONFIG) = 0x0003;       /* open-drain */
    R16(B + O_TIMING) = b->timing;
    R16(B + O_LTIMING) = b->ltiming;
    R16(B + O_HS) = 0;
    R32(b->apdma + 0x0C) = 1;            /* APDMA warm reset */
    spin_us(10);
}

int c64_i2c_init(int bus)
{
    if (bus < 0 || bus > 1)
        return -1;
    struct bus *b = &g_bus[bus];
    if (b->ready)
        return 0;

    R32(b->gate_reg) = 1u << b->gate_bit;        /* ungate main */
    R32(0x1000108C) = 1u << 18;                  /* ungate AP_DMA */
    if (b->arb_reg)
        R32(b->arb_reg) = 1u << b->arb_bit;      /* ungate arbiter */
    __asm__ volatile("dsb sy" ::: "memory");

    c64_u32 m = R32(b->mux_reg);
    R32(b->mux_reg) = (m & ~b->mux_clr) | b->mux_set;

    if (!g_clk_done) {
        /* i2c_sel mux (0=26M, 1=68.25M, 2=124.8M), then /5 by CLOCK_DIV */
        switch (R32(0x100000C0) & 3) {
        case 0: g_bus_clk = 26000000u / 5u; break;
        case 2: g_bus_clk = 124800000u / 5u; break;
        default: g_bus_clk = 68250000u / 5u; break;
        }
        g_clk_done = 1;
    }
    if (!b->khz)
        c64_i2c_set_khz(bus, 400);

    init_hw(b);
    b->ready = 1;
    return 0;
}

/* One PIO transaction. wr: bytes to send (<= 8, starts with the register
 * offset); rd: NULL for a plain write, else nrd (<= 8) bytes read via WRRD. */
int c64_i2c_xfer(int bus, c64_u8 dev, const c64_u8 *wr, int nwr,
                 c64_u8 *rd, int nrd)
{
    if (bus < 0 || bus > 1 || !g_bus[bus].ready || nwr > 8 || nrd > 8)
        return -1;
    struct bus *b = &g_bus[bus];
    c64_u32 B = b->base + b->ch;
    R16(B + O_CLOCK_DIV) = 0x0404;
    R16(B + O_CONTROL) = rd ? 0x003A : 0x0028;   /* ACKERR_DET|CLK_EXT
                                                    (+DIR_CHANGE|RS for WRRD) */
    R16(B + O_EXT_CONF) = 0x8001;
    if (!rd)
        R16(B + O_DELAY_LEN) = 0x0002;
    R16(B + O_IO_CONFIG) = 0x0003;
    R16(B + O_TIMING) = b->timing;
    R16(B + O_LTIMING) = b->ltiming;
    R16(B + O_HS) = 0;
    R16(B + O_HW_TIMEOUT) = 0x018C;
    R16(B + O_SLAVE) = (unsigned short)(dev << 1);
    R16(B + O_INTR_STAT) = 0x01FF;
    R16(B + O_FIFO_ADDR_CLR) = b->ch ? 0x0005 : 0x0001;   /* MCH form if ch */
    R16(B + O_INTR_MASK) = 0;
    R16(B + O_XFER_LEN) = (unsigned short)nwr;
    if (rd) {
        R16(B + O_XFER_LEN_AUX) = (unsigned short)nrd;
        R16(B + O_TRANSAC_LEN) = 2;
    } else {
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
        R16(B + O_FIFO_ADDR_CLR) = b->ch ? 0x0005 : 0x0001;
        init_hw(b);
        if (b->ch)
            R16(b->base + O_START) = 0x0002;     /* resume arbitration */
        return -1;
    }
    for (int i = 0; i < nrd; i++)
        rd[i] = (c64_u8)R16(B + O_DATA);
    return 0;
}

int c64_i2c_write_reg(int bus, c64_u8 dev, c64_u8 reg, c64_u8 val)
{
    c64_u8 wr[2] = { reg, val };
    return c64_i2c_xfer(bus, dev, wr, 2, 0, 0);
}

int c64_i2c_read_reg(int bus, c64_u8 dev, c64_u8 reg)
{
    c64_u8 rd = 0;
    if (c64_i2c_xfer(bus, dev, &reg, 1, &rd, 1) < 0)
        return -1;
    return rd;
}
