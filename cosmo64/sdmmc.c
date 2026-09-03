/* cosmo64/sdmmc.c -- the Cosmo's microSD slot as a block device (M7).
 *
 * MSDC1 at 0x11240000 (msdc@11240000 in the device's own DTB), a 4-bit SD
 * socket with card detect on GPIO 3.
 *
 * THIS IS NOT msdc.c. The eMMC driver next door is a command issuer and says
 * so at length: LK brought MSDC0 up, ran the card through its init sequence,
 * tuned the pads and then read our own boot image with it, so all that file
 * has to do is point a live controller at a block. Nothing ever runs a CARD
 * through its init sequence on MSDC1, so this file is the real thing: the
 * public SD initialisation from CMD0 to a card in the transfer state.
 *
 * What the boot DOES leave behind here was more than expected, and the 2026-09-03
 * readbacks are why this paragraph is measured rather than assumed. The
 * preloader had already ungated MSDC1's clock (CG1_STA showed both gates
 * clear), already chosen its source (mux 4, MSDCPLL/2 at 192 MHz), and
 * already muxed the six pads to function 1. So clock_up() and pins_up() are
 * both, on this unit, elaborate no-ops -- and they stay, because none of that
 * is promised, and a driver that only works when somebody else did its
 * groundwork is a driver that fails on the next boot path. What the boot does
 * NOT do is switch on the card's rails; see THE RAILS below.
 *
 * WHY IT IS WORTH IT. Without a writable volume the shell runs on the RAM
 * disk: SHELL.CFG does not survive a reboot, the Control Panel opens at every
 * boot, `.UNO` apps have nowhere to live and URC's `put` has nowhere to put
 * anything. The eMMC cannot supply that -- every partition on it belongs to
 * Android, to Gemian, to the GPT or to the preloader, and the decision of
 * 2026-09-01 was that it stays that way (see msdc.c's GPT walk, which
 * NOTICES Android's userdata and refuses to offer it). The card in the slot
 * is ours, it is already vfat, and nothing else on this device is competing
 * for it.
 *
 * THE RAILS, and the assumption that was wrong. This file first shipped with
 * no PMIC code at all, on LK's own word: msdc_io.c says "Preload and LK need
 * not touch power since it is default on", and msdc_init's "since
 * VEMC/VMC/VMCH are default on". On 2026-09-03 the hardware disagreed, and
 * the line-level diagnostic below said so exactly -- pads muxed, pull-ups on,
 * CMD and all four data lines still reading zero, which happens when a pad
 * has no supply. Trixie on the same machine enumerates the card at 200 MHz
 * and reports both rails as software-controlled LDOs it switches on itself.
 * So VMCH and VMC are NOT default on here, and pmic.c switches them on: 3.0 V
 * for the card, 3.3 V for its I/O. That last number is deliberately not
 * Linux's, which sits at 1.8 V because Linux negotiated UHS signalling and
 * this driver runs default speed.
 *
 * The register map (offsets, bit positions, the divider arithmetic and the
 * "golden" MSDC_PATCH_BIT values) is the MT6771 MSDC programming interface,
 * taken from the hardware's own definitions; the code below is this project's.
 * Nothing is copied from MediaTek's LK sources, which are proprietary and
 * license-incompatible with UnoDOS.
 *
 * PIO, not DMA, for the same reason msdc.c gives: a wild DMA write on this
 * device is a brick, and PIO cannot make one. The cost is CPU time in the
 * FIFO loop, and at the clock this file settles on that is not the bottleneck.
 *
 * CLOCK, and why it is deliberately modest. The source mux is left EXACTLY as
 * the preloader set it -- read, decoded and logged, never written -- because
 * the same topckgen register carries MSDC0's mux and MSDC0 is where the debug
 * log lives; a slip there takes away the only channel that could report it.
 * Only the mux's power-down bit is touched, and only if it is set. From
 * whatever source that leaves, the divider is computed for 400 kHz during
 * identification and for <= 25 MHz afterwards, which is SD default speed --
 * no CMD6 high-speed switch, no UHS, no tuning, no voltage switch. A 4-bit
 * bus at 25 MHz is ~12 MB/s of card, far more than a PIO loop will draw.
 *
 * WRITES ARE FENCED, exactly as on the eMMC: c64_sd_write() refuses any LBA
 * outside the partition this file found. The card's own MBR is somebody
 * else's data too -- it is what makes the card readable in a PC -- and the
 * fence is what keeps a cluster-arithmetic slip from eating it.
 */

#include "cosmo64.h"

#define MSDC1 0x11240000ull
#define R32(off) (*(volatile c64_u32 *)(MSDC1 + (off)))

/* infracfg_ao: module clock gates. Bit set = GATED, so the CLR register is
 * the one that turns a clock on. MSDC1 owns bit 4 (the module) and bit 16
 * (its source). */
#define INFRA_CG1_SET 0x10001088ull
#define INFRA_CG1_CLR 0x1000108Cull
#define INFRA_CG1_STA 0x10001094ull
#define CG1_MSDC1     (1u << 4)
#define CG1_MSDC1_SRC (1u << 16)

/* topckgen CLK_CFG word carrying both MSDC muxes. MSDC1's selector is
 * [18:16] and its power-down bit is 23; MSDC0's live in the same word, which
 * is the whole reason nothing here does a blind write to it. */
#define CLK_CFG_MSDC 0x10000080ull
#define MSDC1_MUX_SHIFT 16
#define MSDC1_MUX_MASK (7u << 16)
#define MSDC1_MUX_PDN (1u << 23)

/* IOCFG_1: the pad block the six MSDC1 pins live in. */
#define IOCFG1 0x11E80000ull
#define PAD_IES   0x00
#define PAD_SMT   0x10
#define PAD_TDSEL0 0x20
#define PAD_TDSEL1 0x30
#define PAD_RDSEL0 0x40
#define PAD_DRV0  0xA0
#define PAD_DRV1  0xB0
#define PAD_PUPD0 0xC0
#define IOP32(off) (*(volatile c64_u32 *)(IOCFG1 + (off)))

/* GPIO pinmux. MSDC1 is six pins in function 1, and they are GPIO 29-34: the
 * MODE registers hold eight pins of four bits each, so GPIO_BASE+0x330 covers
 * 24..31 (its top twelve bits are 29/30/31) and GPIO_BASE+0x340 covers 32..39
 * (its bottom twelve are 32/33/34). Cross-checked against the SoC's per-pin
 * pad tables, which put exactly those six in IOCFG_1 with CLK on IES bit 6,
 * the four data lines on bit 7 and CMD on bit 8 -- the same grouping the
 * masks below use. */
#define GPIO_MODE4 0x10005330ull
#define GPIO_MODE5 0x10005340ull
#define GPIO_MODE0 0x10005300ull        /* pins 0-7, so GPIO 3 at bits 12-15 */
#define GPIO_DIN0  0x10005200ull        /* card detect is GPIO 3 */
#define GP32(a) (*(volatile c64_u32 *)(c64_u64)(a))

/* MSDC register offsets (same block layout as MSDC0) */
#define MSDC_CFG        0x00
#define MSDC_IOCON      0x04
#define MSDC_PS         0x08
#define MSDC_INT        0x0C
#define MSDC_INTEN      0x10
#define MSDC_FIFOCS     0x14
#define MSDC_TXDATA     0x18
#define MSDC_RXDATA     0x1C
#define SDC_CFG         0x30
#define SDC_CMD         0x34
#define SDC_ARG         0x38
#define SDC_STS         0x3C
#define SDC_RESP0       0x40
#define SDC_RESP1       0x44
#define SDC_RESP2       0x48
#define SDC_RESP3       0x4C
#define SDC_BLK_NUM     0x50
#define EMMC_CFG0       0x70
#define MSDC_DAT_RDDLY0 0xF8
#define MSDC_DAT_RDDLY1 0xFC
#define MSDC_DAT_RDDLY2 0x100
#define MSDC_DAT_RDDLY3 0x104
#define MSDC_PATCH_BIT0 0xB0
#define MSDC_PATCH_BIT1 0xB4
#define MSDC_PATCH_BIT2 0xB8
#define MSDC_PAD_TUNE0  0xF0
#define MSDC_PAD_TUNE1  0xF4
#define MSDC_VERSION    0x114

/* MSDC_CFG */
#define CFG_MODE_SDMMC  (1u << 0)
#define CFG_RST         (1u << 2)
#define CFG_PIO         (1u << 3)
#define CFG_CKSTB       (1u << 7)
#define CFG_CKDIV_SHIFT 8
#define CFG_CKDIV_MASK  (0xFFFu << 8)
#define CFG_CKMOD_MASK  (3u << 20)
#define CFG_HS400_MASK  (1u << 22)
#define CFG_STARTBIT    (3u << 23)

/* MSDC_PS. The DAT and CMD fields are the LIVE PIN LEVELS, which makes this
 * register the one measurement that tells three failure modes apart -- see
 * report_lines(). */
#define PS_DAT0_HIGH    (1u << 16)      /* R1b busy is DAT0 held low */
#define PS_DAT_SHIFT    16
#define PS_DAT_MASK     (0xFFu << 16)
#define PS_CMD          (1u << 24)

/* MSDC_FIFOCS */
#define FIFOCS_RXCNT    0xFFu
#define FIFOCS_TXCNT    (0xFFu << 16)
#define FIFOCS_CLR      (1u << 31)

/* MSDC_INT (write-1-to-clear) */
#define INT_CMDRDY      (1u << 8)
#define INT_CMDTMO      (1u << 9)
#define INT_RSPCRCERR   (1u << 10)
#define INT_XFER_COMPL  (1u << 12)
#define INT_DATTMO      (1u << 14)
#define INT_DATCRCERR   (1u << 15)

/* SDC_CFG */
#define SDC_CFG_BUSWIDTH (3u << 16)
#define SDC_CFG_SDIO     (1u << 19)
#define SDC_CFG_SDIOIDE  (1u << 20)
#define SDC_CFG_DTOC     (0xFFu << 24)

/* SDC_CMD fields */
#define CMD_OPC(x)      ((c64_u32)(x) & 0x3F)
#define CMD_RSPTYP(x)   (((c64_u32)(x) & 7u) << 7)
#define CMD_DTYP(x)     (((c64_u32)(x) & 3u) << 11)
#define CMD_RW_WRITE    (1u << 13)
#define CMD_STOP        (1u << 14)
#define CMD_BLKLEN(x)   (((c64_u32)(x) & 0xFFFu) << 16)

/* SDC_STS */
#define STS_SDCBUSY     (1u << 0)
#define STS_CMDBUSY     (1u << 1)

/* The controller's response-type encoding is not the SD spec's numbering:
 * everything that comes back in one 32-bit word (R1, R5, R6, R7) is type 1,
 * R2 is the 128-bit CID/CSD, R3 is the unprotected OCR, and R1b -- R1 plus a
 * busy signal on DAT0 -- is 7. */
#define RSP_NONE 0
#define RSP_R1   1
#define RSP_R2   2
#define RSP_R3   3
#define RSP_R1B  7
#define DTYP_NONE   0
#define DTYP_SINGLE 1
#define DTYP_MULTI  2

#define BLKSZ 512u

/* SD / MMC command set (the public SD Physical Layer numbering) */
#define SD_GO_IDLE          0
#define SD_ALL_SEND_CID     2
#define SD_SEND_RCA         3
#define SD_SELECT_CARD      7
#define SD_SEND_IF_COND     8
#define SD_SEND_CSD         9
#define SD_STOP_TRANSMISSION 12
#define SD_SET_BLOCKLEN     16
#define SD_READ_SINGLE      17
#define SD_READ_MULTI       18
#define SD_WRITE_SINGLE     24
#define SD_APP_CMD          55
#define SD_APP_SET_BUS_WIDTH 6          /* ACMD6 */
#define SD_APP_OP_COND      41          /* ACMD41 */

/* The host's supply window in the OCR's own units: 3.2-3.4 V, which is what
 * VMCH gives, plus HCS to say we understand block addressing. */
#define OCR_VOLTAGE 0x00300000u
#define OCR_HCS     (1u << 30)
#define OCR_BUSY    (1u << 31)
#define OCR_CCS     (1u << 30)

static int g_ready;
static int g_block_addressed;           /* SDHC/SDXC: the arg is an LBA */
static c64_u32 g_rca;
static c64_u64 g_card_sectors;
static c64_u64 g_part_lba, g_part_sectors;
static c64_u32 g_src_hz;                /* the mux's source, as decoded */
static c64_u32 g_bus_hz;                /* what the card is clocked at now */
static c64_u8 g_scratch[BLKSZ];

int c64_sd_ready(void)          { return g_ready; }
c64_u64 c64_sd_part_lba(void)     { return g_part_lba; }
c64_u64 c64_sd_part_sectors(void) { return g_part_sectors; }

/* ---- timing -------------------------------------------------------------- */

static c64_u64 deadline_ms(unsigned ms)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    return c64_cnt_now() + (f / 1000ull) * ms;
}

static int expired(c64_u64 at)
{
    return c64_cnt_now() > at;
}

static void spin_us(c64_u32 us)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    c64_u64 until = c64_cnt_now() + (f / 1000000ull) * us + 1;
    while (c64_cnt_now() < until)
        ;
}

/* ---- clock, pins and pads ------------------------------------------------ */

/* The five sources the MSDC1 mux can select, in selector order. The last is
 * MSDCPLL/2 and MSDCPLL is 384 MHz on this SoC. A selector outside this table
 * means the preloader left something we do not recognise, and the caller
 * treats that as "assume the fastest thing it could be", which makes every
 * divider conservative rather than the card overclocked. */
static const c64_u32 k_src_hz[5] = {
    26000000u, 208000000u, 182000000u, 156000000u, 192000000u
};

static void clock_up(void)
{
    c64_u32 cg = *(volatile c64_u32 *)INFRA_CG1_STA;
    c64_u32 cfg = *(volatile c64_u32 *)CLK_CFG_MSDC;
    c64_u32 sel = (cfg & MSDC1_MUX_MASK) >> MSDC1_MUX_SHIFT;

    c64_logf("sd: clocks before -- CG1_STA=%08x (msdc1 %s, src %s), "
             "CLK_CFG=%08x (mux=%d pdn=%d)\n", cg,
             (cg & CG1_MSDC1) ? "GATED" : "on",
             (cg & CG1_MSDC1_SRC) ? "GATED" : "on", cfg, (int)sel,
             (int)((cfg & MSDC1_MUX_PDN) ? 1 : 0));

    /* Ungate: writing a 1 into the CLR register clears that gate bit. Both
     * the module clock and its source gate have to go. */
    *(volatile c64_u32 *)INFRA_CG1_CLR = CG1_MSDC1 | CG1_MSDC1_SRC;
    __asm__ volatile("dsb sy" ::: "memory");

    /* The mux SELECTION is not touched -- MSDC0's selector shares this word
     * and MSDC0 carries the log. Only the power-down bit, and only if set. */
    if (cfg & MSDC1_MUX_PDN) {
        *(volatile c64_u32 *)CLK_CFG_MSDC = cfg & ~MSDC1_MUX_PDN;
        __asm__ volatile("dsb sy" ::: "memory");
        spin_us(100);
    }

    g_src_hz = (sel < 5) ? k_src_hz[sel] : 208000000u;
    c64_logf("sd: clocks after  -- CG1_STA=%08x CLK_CFG=%08x; source %d MHz "
             "(selector %d%s)\n", *(volatile c64_u32 *)INFRA_CG1_STA,
             *(volatile c64_u32 *)CLK_CFG_MSDC, (int)(g_src_hz / 1000000u),
             (int)sel, sel < 5 ? "" : ", UNKNOWN -- assuming the fastest");
}

static void pins_up(void)
{
    c64_u32 v;

    /* Function 1 (MSDC1) on all six pins. Four bits each: MODE4 bits 20..31
     * are GPIO 29/30/31, MODE5 bits 0..11 are GPIO 32/33/34.
     *
     * ONLY THE LOW THREE BITS OF EACH NIBBLE. The function is a 3-bit field;
     * the fourth bit is the per-field write-enable that this SoC's masked-write
     * alias of these registers uses, and it reads back set. The first version
     * of this wrote a flat 0x111 and cleared it. That turned out to be
     * harmless AND uninformative, because the 2026-09-03 readback showed these
     * pins already sitting at 0x999 -- function 1 with that bit set, i.e. the
     * preloader had ALREADY muxed MSDC1 and this whole write is a no-op. It
     * stays because nothing guarantees that on the next unit; it preserves the
     * bit because clearing a bit whose meaning you have not established is
     * how a no-op becomes a bug. */
    v = GP32(GPIO_MODE4);
    GP32(GPIO_MODE4) = (v & ~0x77700000u) | 0x11100000u;
    v = GP32(GPIO_MODE5);
    GP32(GPIO_MODE5) = (v & ~0x00000777u) | 0x00000111u;

    /* Input enable and Schmitt trigger on CLK/DAT/CMD (pad bits 6, 7, 8). */
    IOP32(PAD_IES) |= 7u << 6;
    IOP32(PAD_SMT) |= 7u << 6;

    /* Drive strength 3 on all six, which is what the device's own pinctrl
     * node asks for: DRV0 holds DAT (28..31) and CLK (24..27), DRV1 holds
     * CMD (0..3). */
    v = IOP32(PAD_DRV0);
    IOP32(PAD_DRV0) = (v & ~0xFF000000u) | 0x33000000u;
    v = IOP32(PAD_DRV1);
    IOP32(PAD_DRV1) = (v & ~0x0000000Fu) | 0x00000003u;

    /* Pulls: CLK 50K down, CMD and all four DAT lines 50K up -- three bits
     * per pin, CLK at 0, DAT3 at 4, CMD at 8, DAT0/2/1 at 12/16/20. An SD bus
     * with floating DAT lines reads its own noise as a start bit. */
    v = IOP32(PAD_PUPD0);
    IOP32(PAD_PUPD0) = (v & ~0x00FFFFFFu) | 0x00222226u;

    /* Delay selects at their reset value: at 25 MHz there is no tuning to do,
     * and a non-zero value here is a tuning result for a speed we do not run
     * at. */
    IOP32(PAD_TDSEL0) &= ~0xFF000000u;
    IOP32(PAD_TDSEL1) &= ~0x0000000Fu;
    IOP32(PAD_RDSEL0) &= ~0x3FFFF000u;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- the one measurement that separates the failure modes ---------------- *
 * When identification fails there are three candidates and they look
 * identical from the protocol side: no card in the slot, the VMCH/VMC rails
 * not actually on (the one thing this driver takes on the vendor's word), or
 * a pinmux that did not take. MSDC_PS reports the live level of CMD and
 * DAT0-3, and pins_up() has just put 50K pull-ups on all five, so:
 *
 *   CMD=1 DAT=1111  the pads are muxed, the rail is up, and something is
 *                   holding the bus idle-high: a card is there and the
 *                   problem is the protocol or the timing.
 *   CMD=0 DAT=0000  nothing is pulling those lines up. Either the mux write
 *                   did not take (the MODE readback below says which) or the
 *                   pads have no supply -- i.e. VMC is off and the PMIC path
 *                   this driver deliberately skipped is needed after all.
 *   mixed           a card is present and driving, or a line is stuck.
 *
 * An empty slot with the rail up still reads all-high, because the pull-ups
 * are the host's own -- so this does not prove a card, it proves the pads.
 * The card-detect GPIO answers the other half. */
static void report_lines(const char *when)
{
    c64_u32 ps = R32(MSDC_PS);
    c64_logf("sd: pins %s -- MSDC_PS=%08x CMD=%d DAT3..0=%d%d%d%d; "
             "MODE4=%08x MODE5=%08x (want ?111xxxxx / xxxxx111), "
             "IES=%08x PUPD=%08x\n", when, ps, (int)((ps & PS_CMD) ? 1 : 0),
             (int)((ps >> (PS_DAT_SHIFT + 3)) & 1),
             (int)((ps >> (PS_DAT_SHIFT + 2)) & 1),
             (int)((ps >> (PS_DAT_SHIFT + 1)) & 1),
             (int)((ps >> PS_DAT_SHIFT) & 1),
             GP32(GPIO_MODE4), GP32(GPIO_MODE5),
             IOP32(PAD_IES), IOP32(PAD_PUPD0));
}

/* Card detect, read properly. GPIO 3's mode has to be 0 (plain GPIO) before
 * its input register means anything, and nothing in the boot path guarantees
 * that -- so put it there, then read. The device tree says cd_level 1, i.e.
 * a card reads HIGH on this board. Reported, not believed: a card that
 * answers wins over a pin that says the slot is empty. */
static int card_detect(void)
{
    c64_u32 m = GP32(GPIO_MODE0);
    c64_logf("sd: card-detect GPIO 3 mode was %d\n", (int)((m >> 12) & 0xF));
    GP32(GPIO_MODE0) = m & ~(0xFu << 12);       /* function 0 = GPIO      */
    GP32(0x10005010ull) &= ~(1u << 3);          /* DIR: input             */
    __asm__ volatile("dsb sy" ::: "memory");
    return (int)((GP32(GPIO_DIN0) >> 3) & 1u);
}

/* Set the card clock. CKMOD 0 divides: a divisor of zero means source/2,
 * anything else means source/4/divisor. Returns the rate actually programmed,
 * which is always <= the rate asked for. */
static c64_u32 set_clock(c64_u32 hz)
{
    c64_u32 div, sclk;

    if (hz >= g_src_hz / 2u) {
        div = 0;
        sclk = g_src_hz / 2u;
    } else {
        div = (g_src_hz + (hz * 4u) - 1u) / (hz * 4u);
        if (div > 0xFFFu)
            div = 0xFFFu;
        sclk = (g_src_hz / 4u) / div;
    }
    c64_u32 v = R32(MSDC_CFG);
    v &= ~(CFG_HS400_MASK | CFG_CKMOD_MASK | CFG_CKDIV_MASK);
    v |= (div << CFG_CKDIV_SHIFT);              /* CKMOD 0: the divider path */
    R32(MSDC_CFG) = v;
    __asm__ volatile("dsb sy" ::: "memory");

    c64_u64 t = deadline_ms(50);
    while (!(R32(MSDC_CFG) & CFG_CKSTB))
        if (expired(t)) {
            c64_logf("sd: card clock never reported stable (CFG=%08x)\n",
                     R32(MSDC_CFG));
            break;
        }
    g_bus_hz = sclk;
    return sclk;
}

static void set_bus_width(int bits)
{
    c64_u32 v = R32(SDC_CFG) & ~SDC_CFG_BUSWIDTH;
    if (bits == 4)
        v |= 1u << 16;
    R32(SDC_CFG) = v;
}

/* ---- the command issuer -------------------------------------------------- */

static c64_u32 g_resp[4];

static int wait_idle(void)
{
    c64_u64 t = deadline_ms(500);
    while (R32(SDC_STS) & (STS_SDCBUSY | STS_CMDBUSY))
        if (expired(t)) {
            c64_logf("sd: controller stuck busy, SDC_STS=%08x\n", R32(SDC_STS));
            return -1;
        }
    return 0;
}

static void fifo_clear(void)
{
    R32(MSDC_FIFOCS) |= FIFOCS_CLR;
    c64_u64 t = deadline_ms(100);
    while (R32(MSDC_FIFOCS) & (FIFOCS_RXCNT | FIFOCS_TXCNT))
        if (expired(t))
            return;
}

/* Issue one command and collect its response. `quiet` suppresses the log line
 * for commands whose failure is an expected answer rather than a fault --
 * CMD8 to a v1 card, and the ACMD41 poll, which is a busy-wait by design. */
static int send_cmd(c64_u32 opc, c64_u32 arg, c64_u32 rsptyp, c64_u32 dtyp,
                    c64_u32 write, c64_u32 nblk, int quiet)
{
    if (wait_idle() < 0)
        return -1;
    R32(MSDC_INT) = 0xFFFFFFFFu;                /* W1C: start from clean */
    fifo_clear();

    if (dtyp != DTYP_NONE)
        R32(SDC_BLK_NUM) = nblk;
    R32(SDC_ARG) = arg;
    R32(SDC_CMD) = CMD_OPC(opc) | CMD_RSPTYP(rsptyp) | CMD_DTYP(dtyp)
                 | (write ? CMD_RW_WRITE : 0u)
                 | (dtyp != DTYP_NONE ? CMD_BLKLEN(BLKSZ) : 0u);

    if (rsptyp == RSP_NONE) {
        /* No response to wait for, but the command still has to leave the
         * controller before the next one is written. */
        spin_us(100);
        return 0;
    }

    c64_u64 t = deadline_ms(500);
    for (;;) {
        c64_u32 st = R32(MSDC_INT);
        if (st & INT_CMDRDY) {
            R32(MSDC_INT) = INT_CMDRDY;
            break;
        }
        if (st & (INT_CMDTMO | INT_RSPCRCERR)) {
            R32(MSDC_INT) = INT_CMDTMO | INT_RSPCRCERR;
            if (!quiet)
                c64_logf("sd: CMD%d arg=%08x failed (%s)\n", (int)opc, arg,
                         (st & INT_CMDTMO) ? "timeout" : "response CRC");
            return -1;
        }
        if (expired(t)) {
            if (!quiet)
                c64_logf("sd: CMD%d arg=%08x no response, INT=%08x STS=%08x\n",
                         (int)opc, arg, st, R32(SDC_STS));
            return -1;
        }
    }

    if (rsptyp == RSP_R2) {
        /* The 128-bit register arrives with its most significant word in
         * RESP3, so it is read back to front to leave g_resp[0] holding the
         * top -- the order every CSD field below is expressed in. */
        g_resp[0] = R32(SDC_RESP3);
        g_resp[1] = R32(SDC_RESP2);
        g_resp[2] = R32(SDC_RESP1);
        g_resp[3] = R32(SDC_RESP0);
    } else {
        g_resp[0] = R32(SDC_RESP0);
    }

    if (rsptyp == RSP_R1B) {
        /* R1b holds DAT0 low until the card is done. There is no interrupt
         * for it; the pin state is in MSDC_PS. */
        t = deadline_ms(2000);
        while (!(R32(MSDC_PS) & PS_DAT0_HIGH))
            if (expired(t)) {
                c64_logf("sd: CMD%d still busy after 2 s\n", (int)opc);
                return -1;
            }
    }
    return 0;
}

/* CMD55 then the application command. Every ACMD in the init sequence goes
 * through here so that a missing CMD55 cannot be the bug. */
static int send_acmd(c64_u32 opc, c64_u32 arg, c64_u32 rsptyp, int quiet)
{
    if (send_cmd(SD_APP_CMD, g_rca << 16, RSP_R1, DTYP_NONE, 0, 0, quiet) < 0)
        return -1;
    return send_cmd(opc, arg, rsptyp, DTYP_NONE, 0, 0, quiet);
}

static int wait_xfer(void)
{
    c64_u64 t = deadline_ms(5000);
    for (;;) {
        c64_u32 st = R32(MSDC_INT);
        if (st & INT_XFER_COMPL) {
            R32(MSDC_INT) = INT_XFER_COMPL;
            return 0;
        }
        if (st & (INT_DATTMO | INT_DATCRCERR)) {
            R32(MSDC_INT) = INT_DATTMO | INT_DATCRCERR;
            c64_logf("sd: data phase failed (%s)\n",
                     (st & INT_DATTMO) ? "timeout" : "data CRC");
            return -1;
        }
        if (expired(t)) {
            c64_logf("sd: data phase hung, INT=%08x FIFOCS=%08x\n",
                     st, R32(MSDC_FIFOCS));
            return -1;
        }
    }
}

/* ---- moving the bytes ---------------------------------------------------- *
 * Byte-at-a-time into the caller's buffer: -mstrict-align is on for every
 * file in this build and a caller's buffer has no alignment guarantee. */

static int drain_fifo(c64_u8 *dst, unsigned nbytes)
{
    unsigned got = 0;
    c64_u64 t = deadline_ms(5000);
    while (got < nbytes) {
        if ((R32(MSDC_FIFOCS) & FIFOCS_RXCNT) < 4) {
            if (R32(MSDC_INT) & (INT_DATTMO | INT_DATCRCERR))
                return -1;                       /* wait_xfer names it */
            if (expired(t)) {
                c64_logf("sd: RX stalled at %d/%d bytes\n", (int)got,
                         (int)nbytes);
                return -1;
            }
            continue;
        }
        c64_u32 w = R32(MSDC_RXDATA);
        dst[got + 0] = (c64_u8)w;
        dst[got + 1] = (c64_u8)(w >> 8);
        dst[got + 2] = (c64_u8)(w >> 16);
        dst[got + 3] = (c64_u8)(w >> 24);
        got += 4;
        t = deadline_ms(5000);                   /* progress refreshes it */
    }
    return 0;
}

/* The card's address unit: an LBA on SDHC/SDXC, a byte offset on the older
 * standard-capacity cards, which cap out at 2 GB and are still in circulation
 * as the cheap card somebody had in a drawer. */
static c64_u32 card_addr(c64_u64 lba)
{
    return g_block_addressed ? (c64_u32)lba : (c64_u32)(lba * BLKSZ);
}

/* One multi-block read. CMD18 streams until CMD12 stops it, which turns N
 * sectors into one command round trip instead of N -- the difference is
 * visible the moment fat.c reads a multi-sector cluster. */
static int read_run(c64_u64 lba, c64_u8 *dst, unsigned nblk)
{
    int rc;
    if (nblk == 1) {
        if (send_cmd(SD_READ_SINGLE, card_addr(lba), RSP_R1, DTYP_SINGLE,
                     0, 1, 0) < 0)
            return -1;
        if (drain_fifo(dst, BLKSZ) < 0)
            return -1;
        return wait_xfer();
    }
    if (send_cmd(SD_READ_MULTI, card_addr(lba), RSP_R1, DTYP_MULTI,
                 0, nblk, 0) < 0)
        return -1;
    rc = drain_fifo(dst, nblk * BLKSZ);
    if (rc == 0)
        rc = wait_xfer();
    /* CMD12 goes out whether the read worked or not: a card left streaming is
     * a card that answers nothing afterwards. */
    if (send_cmd(SD_STOP_TRANSMISSION, 0, RSP_R1B, DTYP_NONE, 0, 0, 0) < 0)
        rc = -1;
    return rc;
}

static int write_block(c64_u64 lba, const c64_u8 *src)
{
    if (send_cmd(SD_WRITE_SINGLE, card_addr(lba), RSP_R1, DTYP_SINGLE,
                 1, 1, 0) < 0)
        return -1;

    unsigned put = 0;
    c64_u64 t = deadline_ms(5000);
    while (put < BLKSZ) {
        /* TXCNT counts bytes already queued; the FIFO is 128 bytes deep. */
        if (((R32(MSDC_FIFOCS) & FIFOCS_TXCNT) >> 16) > (128u - 4u)) {
            if (expired(t)) {
                c64_logf("sd: TX stalled at %d/%d bytes\n", (int)put, BLKSZ);
                return -1;
            }
            continue;
        }
        c64_u32 w = (c64_u32)src[put]
                  | ((c64_u32)src[put + 1] << 8)
                  | ((c64_u32)src[put + 2] << 16)
                  | ((c64_u32)src[put + 3] << 24);
        R32(MSDC_TXDATA) = w;
        put += 4;
        t = deadline_ms(5000);
    }
    return wait_xfer();
}

/* ---- the public block interface ------------------------------------------ */

/* One command can stream at most this many sectors. The cap is not the
 * card's -- it is the FIFO loop's: a run this long is ~64 KB of PIO with the
 * card clock stalled whenever we fall behind, and a shorter run bounds how
 * long a single c64_sd_read() can sit inside the shell's poll loop. */
#define MAX_RUN 64u

int c64_sd_read(c64_u64 lba, void *buf, unsigned nblk)
{
    if (!g_ready || !nblk)
        return -1;
    if (lba + nblk > g_card_sectors || lba + nblk < lba)
        return -1;
    c64_u8 *p = (c64_u8 *)buf;
    while (nblk) {
        unsigned n = nblk > MAX_RUN ? MAX_RUN : nblk;
        if (read_run(lba, p, n) < 0)
            return -1;
        lba += n;
        p += (c64_u64)n * BLKSZ;
        nblk -= n;
    }
    return 0;
}

int c64_sd_write(c64_u64 lba, const void *buf, unsigned nblk)
{
    if (!g_ready || !nblk)
        return -1;
    /* THE FENCE. The card's partition table and anything outside our
     * partition are somebody else's -- the card is meant to stay readable in
     * a PC. A write outside the partition is a bug in the caller, and it is
     * refused here rather than trusted anywhere above. */
    if (!g_part_sectors || lba < g_part_lba
        || lba + nblk > g_part_lba + g_part_sectors || lba + nblk < lba) {
        c64_logf("sd: REFUSED write of %d blocks at LBA %d -- outside the "
                 "partition (%d..%d)\n", (int)nblk, (int)lba, (int)g_part_lba,
                 (int)(g_part_lba + g_part_sectors));
        return -1;
    }
    const c64_u8 *p = (const c64_u8 *)buf;
    for (unsigned i = 0; i < nblk; i++)
        if (write_block(lba + i, p + (c64_u64)i * BLKSZ) < 0)
            return -1;
    return 0;
}

/* ---- the card initialisation sequence ------------------------------------ */

static c64_u64 csd_sectors(void)
{
    /* CSD structure version is the top two bits. Version 1 (standard
     * capacity) states a size in device-specific units and a size multiplier;
     * version 2 (SDHC/SDXC) states it directly in 512 KB units, which is the
     * one nearly every card in the last fifteen years uses. */
    c64_u32 ver = g_resp[0] >> 30;
    if (ver == 1) {
        c64_u32 csize = ((g_resp[1] & 0x3Fu) << 16) | (g_resp[2] >> 16);
        return ((c64_u64)csize + 1ull) * 1024ull;
    }
    if (ver == 0) {
        c64_u32 csize = ((g_resp[1] & 0x3FFu) << 2) | (g_resp[2] >> 30);
        c64_u32 mult = ((g_resp[2] >> 15) & 7u) + 2u;
        c64_u32 rdlen = (g_resp[1] >> 16) & 0xFu;
        c64_u64 bytes = ((c64_u64)csize + 1ull) << (mult + rdlen);
        return bytes / BLKSZ;
    }
    c64_logf("sd: CSD structure version %d is not one this driver knows\n",
             (int)ver);
    return 0;
}

static int card_identify(void)
{
    int v2;

    /* Reset to idle. CMD0 has no response, so the only evidence it landed is
     * that CMD8 answers afterwards. */
    send_cmd(SD_GO_IDLE, 0, RSP_NONE, DTYP_NONE, 0, 0, 0);
    spin_us(2000);

    /* CMD8 with a 2.7-3.6 V pattern. A v2 card echoes the check pattern; a v1
     * card does not answer at all, which is an expected answer and not a
     * failure -- hence the quiet flag. */
    v2 = send_cmd(SD_SEND_IF_COND, 0x1AAu, RSP_R1, DTYP_NONE, 0, 0, 1) == 0
         && (g_resp[0] & 0xFFu) == 0xAAu;
    c64_logf("sd: CMD8 %s -- %s card\n",
             v2 ? "echoed 0x1AA" : "no answer",
             v2 ? "v2 (SDHC/SDXC capable)" : "v1 (standard capacity)");

    /* ACMD41 until the card leaves its power-up busy state. The spec allows a
     * full second; give it two, and poll gently so a card that is genuinely
     * absent costs a bounded wait rather than a wedge. */
    c64_u64 t = deadline_ms(2000);
    c64_u32 arg = OCR_VOLTAGE | (v2 ? OCR_HCS : 0u);
    g_rca = 0;
    for (;;) {
        if (send_acmd(SD_APP_OP_COND, arg, RSP_R3, 1) == 0
            && (g_resp[0] & OCR_BUSY))
            break;
        if (expired(t)) {
            c64_logf("sd: no card answered ACMD41 within 2 s (last OCR %08x)"
                     " -- empty slot, or the rails are not up\n", g_resp[0]);
            return -1;
        }
        spin_us(10000);
    }
    g_block_addressed = v2 && (g_resp[0] & OCR_CCS) != 0;
    c64_logf("sd: OCR=%08x -- %s addressing\n", g_resp[0],
             g_block_addressed ? "block (SDHC/SDXC)" : "byte (standard capacity)");

    if (send_cmd(SD_ALL_SEND_CID, 0, RSP_R2, DTYP_NONE, 0, 0, 0) < 0)
        return -1;
    c64_logf("sd: CID %08x %08x %08x %08x\n", g_resp[0], g_resp[1],
             g_resp[2], g_resp[3]);

    /* The card picks its own relative address and hands it back in the top
     * half of the response. Everything from here on is addressed by it. */
    if (send_cmd(SD_SEND_RCA, 0, RSP_R1, DTYP_NONE, 0, 0, 0) < 0)
        return -1;
    g_rca = g_resp[0] >> 16;
    if (!g_rca) {
        c64_log("sd: the card published RCA 0, which is the broadcast "
                "address -- refusing to address it\n");
        return -1;
    }

    if (send_cmd(SD_SEND_CSD, g_rca << 16, RSP_R2, DTYP_NONE, 0, 0, 0) < 0)
        return -1;
    g_card_sectors = csd_sectors();
    if (!g_card_sectors)
        return -1;
    c64_logf("sd: RCA %04x, %d sectors (%d MiB)\n", (int)g_rca,
             (int)g_card_sectors, (int)(g_card_sectors / 2048));

    /* Standby -> transfer. R1b: the card holds DAT0 low while it gets there. */
    if (send_cmd(SD_SELECT_CARD, g_rca << 16, RSP_R1B, DTYP_NONE, 0, 0, 0) < 0)
        return -1;

    /* Four data lines. The card has to agree first (ACMD6) and the controller
     * second, in that order -- swap them and the next command is read off
     * three lines that are not driving yet. */
    if (send_acmd(SD_APP_SET_BUS_WIDTH, 2, RSP_R1, 0) < 0) {
        c64_log("sd: ACMD6 refused -- staying on a 1-bit bus\n");
    } else {
        set_bus_width(4);
        c64_log("sd: bus width 4\n");
    }

    /* Block length. Block-addressed cards fix it at 512 and ignore this, but
     * a standard-capacity card does not. */
    if (send_cmd(SD_SET_BLOCKLEN, BLKSZ, RSP_R1, DTYP_NONE, 0, 0, 0) < 0)
        return -1;

    /* Up to SD default speed. Deliberately NOT the high-speed switch: CMD6
     * needs a 64-byte data phase of its own, and 25 MHz on four lines is
     * already more card than this PIO loop will consume. */
    c64_logf("sd: card clock %d kHz\n", (int)(set_clock(25000000u) / 1000u));
    return 0;
}

/* ---- finding the volume -------------------------------------------------- *
 * A card formatted by anything modern carries an MBR with one FAT partition;
 * a card formatted as a "superfloppy" has a BPB at LBA 0 and no table at all.
 * Both shapes are in the wild and both are handled, because the alternative
 * is telling somebody their card is broken when a camera formatted it. */

static c64_u32 le32(const c64_u8 *p)
{
    return (c64_u32)p[0] | ((c64_u32)p[1] << 8) | ((c64_u32)p[2] << 16)
         | ((c64_u32)p[3] << 24);
}

static int looks_like_bpb(const c64_u8 *s)
{
    c64_u32 bps = (c64_u32)s[11] | ((c64_u32)s[12] << 8);
    return s[510] == 0x55 && s[511] == 0xAA && bps == BLKSZ && s[13] != 0
        && (s[0] == 0xEB || s[0] == 0xE9);
}

static int find_partition(void)
{
    if (c64_sd_read(0, g_scratch, 1) < 0) {
        c64_log("sd: LBA 0 unreadable\n");
        return -1;
    }
    c64_logf("sd: LBA 0 signature %02x%02x\n", g_scratch[510], g_scratch[511]);

    /* Superfloppy first: a BPB at LBA 0 is unambiguous, and a partition entry
     * decoded out of BPB bytes would be nonsense pointing anywhere. */
    if (looks_like_bpb(g_scratch)) {
        g_part_lba = 0;
        g_part_sectors = g_card_sectors;
        c64_log("sd: no partition table -- the whole card is one FAT volume "
                "(superfloppy)\n");
        return 0;
    }

    if (g_scratch[510] != 0x55 || g_scratch[511] != 0xAA) {
        c64_log("sd: LBA 0 is neither a BPB nor a partition table -- the card "
                "is unformatted, or formatted as something this does not read\n");
        return -1;
    }

    /* The four primary entries at offset 0x1BE, sixteen bytes each: type at
     * +4, first LBA at +8, sector count at +12. The first FAT-shaped entry
     * wins; extended partitions are not walked, because a card with a
     * partition chain on it is not a card anybody formatted for a camera. */
    for (int i = 0; i < 4; i++) {
        const c64_u8 *e = g_scratch + 0x1BE + i * 16;
        c64_u8 type = e[4];
        c64_u64 first = le32(e + 8), count = le32(e + 12);
        if (!type || !count)
            continue;
        c64_logf("sd: MBR entry %d: type %02x, LBA %d, %d sectors (%d MiB)\n",
                 i, type, (int)first, (int)count, (int)(count / 2048));
        if (first + count > g_card_sectors || first + count < first)
            continue;                            /* the table lies; skip it */
        /* FAT12/16/32 in every flavour a card is likely to carry. */
        if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B
            || type == 0x0C || type == 0x0E) {
            if (g_part_sectors)
                continue;                        /* already took the first */
            g_part_lba = first;
            g_part_sectors = count;
        }
    }
    if (!g_part_sectors) {
        c64_log("sd: the partition table holds no FAT partition\n");
        return -1;
    }
    c64_logf("sd: volume at LBA %d, %d sectors (%d MiB)\n", (int)g_part_lba,
             (int)g_part_sectors, (int)(g_part_sectors / 2048));
    return 0;
}

/* ---- bring-up ------------------------------------------------------------ */

void c64_sd_init(void)
{
    /* Idempotent for the same reason c64_blk_init() is: c_main brings storage
     * up before the shell and blk.c asks again on its way to mounting, so
     * neither has to assume it runs first -- and a second attempt after a
     * FAILED one would re-run a probe that already spent its timeouts. */
    static int g_inited;
    if (g_inited)
        return;
    g_inited = 1;

    clock_up();

    /* Card detect is a plain GPIO on this board, not the controller's own
     * CDSTS. It is REPORTED and not believed: if it says empty and a card
     * answers anyway, the card wins. */
    c64_logf("sd: card-detect GPIO 3 reads %d (cd_level 1 = card present)\n",
             card_detect());

    /* The pads, before and after. Between these two lines is the entire
     * question of whether the mux write took, and what the bus looks like
     * once it has -- see report_lines(). */
    report_lines("before pins_up");
    pins_up();
    report_lines("after pins_up ");

    /* THE RAILS. This is what the 2026-09-03 boot proved missing: the pads
     * were muxed and pulled up and still read zero, because VMCH and VMC were
     * off. pmic.c switches them on; the line below is the proof, and it comes
     * before a single command is sent. CMD=1 DAT3..0=1111 here means the bus
     * is alive and anything that fails afterwards is protocol or timing. */
    c64_pmic_init();
    c64_pmic_sd_rails_on();
    report_lines("after rails  ");

    c64_logf("sd: MSDC1 before reset: CFG=%08x SDC_CFG=%08x STS=%08x ver=%08x\n",
             R32(MSDC_CFG), R32(SDC_CFG), R32(SDC_STS), R32(MSDC_VERSION));
    if (R32(MSDC_VERSION) == 0 || R32(MSDC_VERSION) == 0xFFFFFFFFu) {
        c64_log("sd: MSDC1 reads as dead silicon -- the clock never arrived\n");
        return;
    }

    /* Unlike msdc.c, this controller IS reset: nothing has configured it, so
     * there is no adopted tuning to throw away and a known state is worth
     * more than whatever the last boot left. */
    R32(MSDC_INTEN) = 0;
    R32(MSDC_CFG) |= CFG_MODE_SDMMC | CFG_PIO;
    R32(MSDC_CFG) |= CFG_RST;
    {
        c64_u64 t = deadline_ms(100);
        while (R32(MSDC_CFG) & CFG_RST)
            if (expired(t))
                break;
    }
    R32(MSDC_CFG) |= CFG_MODE_SDMMC | CFG_PIO;   /* again: the reset may have
                                                  * taken them with it */
    fifo_clear();
    R32(MSDC_INT) = 0xFFFFFFFFu;

    /* The timing registers, at the values this SoC's own driver resets them
     * to. They are written rather than left alone because a reset does not
     * clear all of them and a stale delay-line value is a data CRC error
     * nobody would connect to this line. PATCH_BIT2 additionally gets the
     * response-wait count the platform wants (bits 2-3 = 3) and has its 64 GB
     * addressing bit cleared -- this is a card, not a 64 GB eMMC array. */
    R32(MSDC_IOCON) = 0;
    R32(MSDC_DAT_RDDLY0) = 0;
    R32(MSDC_DAT_RDDLY1) = 0;
    R32(MSDC_DAT_RDDLY2) = 0;
    R32(MSDC_DAT_RDDLY3) = 0;
    R32(MSDC_PAD_TUNE0) = 0;
    R32(MSDC_PAD_TUNE1) = 0;
    R32(MSDC_PATCH_BIT0) = 0x403C0006u;
    R32(MSDC_PATCH_BIT1) = 0xFFE20349u;
    R32(MSDC_PATCH_BIT2) = 0x1480180Du;
    R32(MSDC_CFG) &= ~CFG_STARTBIT;             /* start bit on the rising edge */
    R32(EMMC_CFG0) &= ~(1u << 15);              /* no eMMC boot support here  */

    /* SDIO mode on and its interrupt detection off is what this controller
     * wants for ordinary SD too; the maximum data timeout costs nothing and
     * saves a slow card from a spurious DATTMO. */
    R32(SDC_CFG) = (R32(SDC_CFG) | SDC_CFG_SDIO | SDC_CFG_DTOC)
                 & ~SDC_CFG_SDIOIDE;
    set_bus_width(1);

    /* Identification runs at 400 kHz or below, on one data line, per the SD
     * spec -- the card does not know the bus is good for more yet. */
    c64_logf("sd: identification clock %d kHz\n",
             (int)(set_clock(400000u) / 1000u));
    /* The spec's power-up wait: at least 1 ms and 74 clocks after the supply
     * is stable. The supply has been stable since the PMIC came up, but the
     * clock has only just started, so the clocks are what this buys. */
    spin_us(2000);

    __asm__ volatile("dsb sy" ::: "memory");
    report_lines("at CMD0     ");
    g_ready = 1;                                 /* the issuer needs it set */
    if (card_identify() < 0) {
        g_ready = 0;
        c64_log("sd: no usable card\n");
        return;
    }
    if (find_partition() < 0) {
        /* Reads still work -- the card is fine, it just has no volume we can
         * mount. Keep the driver up so a future mkfs path has a transport. */
        c64_log("sd: readable, but no FAT volume -- nothing to mount\n");
        return;
    }
    c64_logf("sd: ready -- %d MiB card, %d MiB volume, 4-bit at %d kHz\n",
             (int)(g_card_sectors / 2048), (int)(g_part_sectors / 2048),
             (int)(g_bus_hz / 1000u));
}
