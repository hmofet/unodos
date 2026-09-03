/* cosmo64/pmic.c -- the MT6358 PMIC, through MediaTek's PMIC wrapper (M7).
 *
 * WHY THIS EXISTS, AND WHY IT DID NOT UNTIL NOW. sdmmc.c shipped with one
 * assumption taken on the vendor's word instead of measured: LK's own
 * msdc_io.c says "Preload and LK need not touch power since it is default
 * on", and msdc_init's "since VEMC/VMC/VMCH are default on". On 2026-09-03
 * the hardware said otherwise, and said it precisely. With the MSDC1 pads
 * muxed (they already were -- the preloader had done it) and 50K pull-ups
 * verified on CMD and all four data lines, MSDC_PS read:
 *
 *     CMD=0 DAT3..0=0000
 *
 * A pad with a pull-up enabled reads zero only if something drives it low or
 * the pad has no I/O supply, and nothing drives an idle bus. Meanwhile Trixie
 * on the same machine enumerates the card happily -- a 29 GB SD at 200 MHz
 * SDR104 -- and reports `vmch: enabled 3.0 V, vmc: enabled 1.8 V`. Those are
 * software-controlled LDOs that Linux switches on when it probes MSDC1, and
 * nothing in our boot path switches them on at all. That is the whole bug.
 *
 * So: the PMIC wrapper, which is the only way to reach an MT6358.
 *
 * THIS IS AN ADOPTION, NOT A BRING-UP -- the same shortcut msdc.c takes with
 * MSDC0, and for the same reason. A full PWRAP bring-up is the dual-IO
 * handshake, the key sequence, the calibration and the interface tables, and
 * none of it is needed here: the preloader talks to this PMIC constantly
 * (battery, every rail on the board) and LK does too, so the wrapper is live
 * and WACS2 -- the channel the AP gets -- is enabled at handover. This file
 * checks that it is, reads the chip ID to prove the path end to end, and
 * otherwise only issues commands.
 *
 * If WACS2 turns out NOT to be enabled, this refuses rather than attempting a
 * blind bring-up. A half-initialised PMIC wrapper is not a driver that fails,
 * it is a phone that does not come back.
 *
 * WRITES ARE FENCED TO A WHITELIST, and this fence is stricter than msdc.c's
 * for a blunt reason: an eMMC write can destroy the software on the device,
 * but a PMIC write can destroy the device. Every rail on this board is behind
 * these registers -- the CPU core voltage, DRAM, the modem -- and a slip is
 * not a corrupted partition, it is silicon at a voltage it was not built for.
 * There is no plausible caller for "write an arbitrary PMIC register", so the
 * capability simply does not exist here: c64_pmic_write() takes an index into
 * a table of four registers that this port has a reason to touch, and there
 * is no other way through.
 *
 * The register map (the wrapper's WACS2 channel, the MT6358 addresses and the
 * voltage selector tables) is hardware fact, cross-checked between the
 * MT6771 wrapper definitions, the MT6358 register list and the running
 * device's own sysfs; the code is this project's. Nothing is copied from
 * MediaTek's LK sources.
 */

#include "cosmo64.h"

/* ---- the wrapper --------------------------------------------------------- */

#define PWRAP 0x1000D000ull
#define W32(off) (*(volatile c64_u32 *)(PWRAP + (off)))

#define WACS2_EN     0x090               /* bit 0: the AP's channel is on   */
#define WACS2_CMD    0xC20
#define WACS2_RDATA  0xC24
#define WACS2_VLDCLR 0xC28

/* WACS2_RDATA: [15:0] the data, [18:16] the state machine, [19] req */
#define RDATA_DATA(x) ((x) & 0xFFFFu)
#define RDATA_FSM(x)  (((x) >> 16) & 7u)
#define FSM_IDLE      0u
#define FSM_WFVLDCLR  6u                 /* data ready, waiting to be cleared */

/* ---- MT6358 registers this port has a reason to know about --------------- */

#define MT6358_SWCID         0x000Au     /* chip id; the top byte is 0x58    */
#define MT6358_LDO_VMC_CON0  0x1CC4u     /* bit 0 = enable                   */
#define MT6358_LDO_VMCH_CON0 0x1CD8u
#define MT6358_VMCH_ANA_CON0 0x1E48u     /* bits 10:8 = voltage selector     */
#define MT6358_VMC_ANA_CON0  0x1E4Cu     /* bits 11:8 = voltage selector     */

/* Voltage selectors, from the MT6358's own tables. Not a linear encoding --
 * these are the only legal values and the gaps between them are not voltages.
 *
 * VMCH is the card's supply and VMC its I/O rail. The board runs VMCH at
 * 3.0 V (both LK and Linux choose it), so this does too. VMC is the one where
 * copying Linux would have been WRONG: Linux leaves it at 1.8 V because it
 * negotiated UHS SDR104 signalling, and this driver deliberately runs SD
 * default speed, whose signalling level is 3.3 V. */
#define VMCH_SEL_2V9 2u
#define VMCH_SEL_3V0 3u
#define VMCH_SEL_3V3 5u
#define VMC_SEL_1V8  4u
#define VMC_SEL_2V9  10u
#define VMC_SEL_3V0  11u
#define VMC_SEL_3V3  13u

/* THE WHITELIST. The index is the API; the address is private. Adding a row
 * is a deliberate act with a reason written beside it, which is the point --
 * see the header on why this is a table and not an address parameter. */
static const struct { c64_u32 addr; const char *name; } k_wr[] = {
    { MT6358_LDO_VMCH_CON0, "VMCH_CON0" },   /* SD card supply, on/off      */
    { MT6358_VMCH_ANA_CON0, "VMCH_ANA"  },   /* ...and its voltage          */
    { MT6358_LDO_VMC_CON0,  "VMC_CON0"  },   /* SD I/O rail, on/off         */
    { MT6358_VMC_ANA_CON0,  "VMC_ANA"   },   /* ...and its voltage          */
};
#define WR_VMCH_EN  0
#define WR_VMCH_ANA 1
#define WR_VMC_EN   2
#define WR_VMC_ANA  3
#define WR_COUNT    (int)(sizeof k_wr / sizeof k_wr[0])

static int g_ready;

/* ---- timing -------------------------------------------------------------- */

static c64_u64 deadline_ms(unsigned ms)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    return c64_cnt_now() + (f / 1000ull) * ms;
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

static int wait_fsm(c64_u32 want, c64_u32 *last)
{
    c64_u64 t = deadline_ms(50);
    for (;;) {
        c64_u32 r = W32(WACS2_RDATA);
        if (RDATA_FSM(r) == want) {
            if (last)
                *last = r;
            return 0;
        }
        if (c64_cnt_now() > t) {
            if (last)
                *last = r;
            return -1;
        }
    }
}

/* ---- one WACS2 transaction ----------------------------------------------- *
 * The command word is (write << 31) | ((addr >> 1) << 16) | data: the address
 * is halved because the MT6358's registers are 16 bits wide and its map is
 * byte-addressed, so every legal address is even and the low bit carries no
 * information. */
static int wacs2(int write, c64_u32 addr, c64_u32 data, c64_u32 *out)
{
    c64_u32 r = 0;

    if (!g_ready && addr != MT6358_SWCID)
        return -1;
    if (addr & 1u)
        return -1;

    if (wait_fsm(FSM_IDLE, &r) < 0) {
        c64_logf("pmic: wrapper not idle before %s of %04x (RDATA=%08x)\n",
                 write ? "a write" : "a read", addr, r);
        return -1;
    }
    W32(WACS2_CMD) = ((c64_u32)(write ? 1u : 0u) << 31)
                   | ((addr >> 1) << 16) | (data & 0xFFFFu);
    __asm__ volatile("dsb sy" ::: "memory");

    if (!write) {
        if (wait_fsm(FSM_WFVLDCLR, &r) < 0) {
            c64_logf("pmic: read of %04x never produced data (RDATA=%08x)\n",
                     addr, r);
            return -1;
        }
        if (out)
            *out = RDATA_DATA(r);
        W32(WACS2_VLDCLR) = 1;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    return 0;
}

int c64_pmic_read(c64_u32 addr, c64_u32 *val)
{
    return wacs2(0, addr, 0, val);
}

/* The only way to write. `idx` indexes the whitelist above; there is no
 * address parameter, so there is no way to reach a rail this port has not
 * thought about. */
static int pmic_write(int idx, c64_u32 val)
{
    if (idx < 0 || idx >= WR_COUNT)
        return -1;
    return wacs2(1, k_wr[idx].addr, val, 0);
}

/* Read, change the masked field, write back, and read again to confirm. The
 * read-back is not ceremony: this is a serial bus to another chip, and "the
 * write returned success" only means the wrapper accepted the command. */
static int pmic_rmw(int idx, c64_u32 mask, c64_u32 val)
{
    c64_u32 cur = 0, want, back = 0;

    if (idx < 0 || idx >= WR_COUNT)
        return -1;
    if (c64_pmic_read(k_wr[idx].addr, &cur) < 0)
        return -1;
    want = (cur & ~mask) | (val & mask);
    if (want != cur && pmic_write(idx, want) < 0)
        return -1;
    if (c64_pmic_read(k_wr[idx].addr, &back) < 0)
        return -1;
    if ((back & mask) != (val & mask)) {
        c64_logf("pmic: %s did not take -- wanted %04x in mask %04x, read "
                 "back %04x\n", k_wr[idx].name, val, mask, back);
        return -1;
    }
    return 0;
}

/* ---- bring-up ------------------------------------------------------------ */

void c64_pmic_init(void)
{
    static int done;
    c64_u32 en = 0, id = 0;

    if (done)
        return;
    done = 1;

    en = W32(WACS2_EN);
    if (!(en & 1u)) {
        c64_logf("pmic: WACS2 is DISABLED at handover (EN=%08x) -- the "
                 "wrapper was never brought up, and this driver will not "
                 "attempt it blind\n", en);
        return;
    }

    /* The chip ID proves the whole path in one read: the wrapper is idle, it
     * accepts a command, the serial link to the PMIC works, and the thing on
     * the other end is the part we think it is. */
    if (wacs2(0, MT6358_SWCID, 0, &id) < 0) {
        c64_log("pmic: the wrapper is enabled but would not answer a read\n");
        return;
    }
    if ((id >> 8) != 0x58u) {
        c64_logf("pmic: chip id reads %04x -- not an MT6358 (want 58xx). "
                 "Refusing to write rails on a part this does not know\n", id);
        return;
    }
    g_ready = 1;
    c64_logf("pmic: MT6358 answering over the wrapper (SWCID=%04x, "
             "WACS2_EN=%08x)\n", id, en);
}

int c64_pmic_present(void)
{
    return g_ready;
}

/* ---- the SD card's two rails --------------------------------------------- */

void c64_pmic_sd_rails_on(void)
{
    c64_u32 a = 0, b = 0, c = 0, d = 0;

    if (!g_ready) {
        c64_log("sd: no PMIC -- the card's rails cannot be switched on\n");
        return;
    }

    /* What the preloader left, before anything here changes it. This is the
     * line that settles whether LK's "default on" was ever true on this
     * board; on 2026-09-03 the pads said it was not. */
    c64_pmic_read(MT6358_LDO_VMCH_CON0, &a);
    c64_pmic_read(MT6358_VMCH_ANA_CON0, &b);
    c64_pmic_read(MT6358_LDO_VMC_CON0, &c);
    c64_pmic_read(MT6358_VMC_ANA_CON0, &d);
    c64_logf("sd: rails as found -- VMCH en=%d sel=%d, VMC en=%d sel=%d\n",
             (int)(a & 1u), (int)((b >> 8) & 7u),
             (int)(c & 1u), (int)((d >> 8) & 0xFu));

    /* Voltage BEFORE enable, both times: a rail switched on at whatever
     * selector was left behind is a rail at the wrong voltage for however
     * long the second write takes. */
    if (pmic_rmw(WR_VMCH_ANA, 0x0700u, VMCH_SEL_3V0 << 8) < 0
        || pmic_rmw(WR_VMCH_EN, 0x0001u, 1) < 0) {
        c64_log("sd: could not bring VMCH (the card's supply) up\n");
        return;
    }
    if (pmic_rmw(WR_VMC_ANA, 0x0F00u, VMC_SEL_3V3 << 8) < 0
        || pmic_rmw(WR_VMC_EN, 0x0001u, 1) < 0) {
        c64_log("sd: could not bring VMC (the card's I/O rail) up\n");
        return;
    }

    /* The device tree asks for 60 us of ramp on both of these. Give it two
     * orders of magnitude more and call it free: this happens once, at boot,
     * and the SD specification wants a millisecond of settled supply before
     * the first command anyway. */
    spin_us(10000);
    c64_log("sd: VMCH 3.0 V and VMC 3.3 V are on\n");
}
