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

/* THE WRITE GATE. It shipped OFF, and the reason it is now on is worth
 * keeping: a PMIC write is the one class of mistake on this device that does
 * not come back -- every rail is behind these registers, and a wrong address
 * is not a corrupted partition but silicon at a voltage it was not built for.
 * So the capability was compiled out entirely until a read-only build had
 * confirmed the address map on the actual hardware.
 *
 * It did, on 2026-09-03, three independent ways: the VEMC control read
 * enabled at 3.0 V as a working eMMC's rail must; every selector decoded to
 * one of the three or four legal codes its field allows; and VEMC and VMCH
 * both matched, to the millivolt, what the running Linux kernel reports for
 * the same two rails through its own decoded sysfs. The next boot with the
 * writes armed brought the card up first try.
 *
 * So the default is 1: a build that cannot switch these two rails on is a
 * build with no SD card, and that is no longer a trade worth making. What
 * protects the device now is not the absence of the code but the checks
 * around it -- the whitelist with no address parameter, MAP CONFIRMED gating
 * every write, the voltage sanity gate, and the read-back verify. Those are
 * the durable protections; this switch was a one-time precaution for an
 * unverified map.
 *
 * PMIC_WRITE=0 ./build.sh shell still builds an image that physically cannot
 * write the PMIC, which is the thing to reach for on a NEW unit, or after
 * touching any address in the table above. */
#ifndef C64_PMIC_WRITE
#define C64_PMIC_WRITE 1
#endif

/* AUDIO IS OFF BY DEFAULT and gated on C64_PMIC_WRITE as well, below.
 * The codec set is twenty-three addresses this port had never written
 * before; a shipped image has no reason to carry the instructions that
 * write them, and PMIC_WRITE=0 must keep meaning what its own header
 * says it means -- an image that CANNOT write the PMIC, not one that
 * declines to. */
#ifndef C64_AUDIO
#define C64_AUDIO 0
#endif

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

/* VEMC is the eMMC's rail, and it is read here for ONE reason: it is the
 * control. The eMMC demonstrably works under this payload -- msdc.c reads the
 * GPT off it on every boot, and the log two screens up proves it -- so VEMC
 * MUST read enabled. If it does, and it reports a legal voltage, then these
 * addresses land where this file thinks they do, and the same map's reading
 * of VMCH and VMC can be believed. If VEMC reads disabled, the map is wrong,
 * and the correct response is to write nothing at all.
 *
 * A wrong address does not usually announce itself. This is how it is made
 * to: an experiment with a known answer, run beside the one whose answer we
 * do not know. */
#define MT6358_LDO_VEMC_CON0 0x1B1Cu
#define MT6358_VEMC_ANA_CON0 0x1E38u

/* Voltage selectors, from the MT6358's own tables. Not a linear encoding --
 * these are the only legal values and the gaps between them are not voltages.
 *
 * VMCH is the card's supply and VMC its I/O rail, and the read-only pass of
 * 2026-09-03 settled what to do with both: the preloader leaves them switched
 * OFF but already selected at 3.0 V, which is squarely inside default speed's
 * 2.7-3.6 V window and is what VMCH runs at anyway. So this driver changes no
 * voltage at all. It sets two enable bits and nothing else, which is the
 * smallest intervention that can work on a chip where the cost of a wrong
 * write is the whole device.
 *
 * Copying the live Linux values would have been WRONG twice over: Linux keeps
 * VMC at 1.8 V because it negotiated UHS SDR104 signalling, and this driver
 * runs default speed. Reading the hardware beat both guessing and copying. */
#define VMCH_SEL_2V9 2u
#define VMCH_SEL_3V0 3u
#define VMCH_SEL_3V3 5u
#define VMC_SEL_1V8  4u
#define VMC_SEL_2V9  10u
#define VMC_SEL_3V0  11u
#define VMC_SEL_3V3  13u

#if C64_PMIC_WRITE
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
#endif  /* C64_PMIC_WRITE */

/* ---- the audio codec set (C64_AUDIO) -------------------------------------
 * A SECOND TABLE, BEHIND A SECOND FLAG, and both of those are the point. The
 * whitelist above exists because a wrong PMIC address is silicon at a voltage
 * it was not built for; audio needs twenty-three more addresses, which is a
 * large fraction again of everything this port has ever written to this chip.
 * Keeping them in their own table under their own #if means the shipped image
 * cannot write one of them, and that `AUDIO=1` is a deliberate act rather
 * than a default.
 *
 * WHAT MAKES THESE DEFENSIBLE, given there is no datasheet here. They are not
 * derived, they are OBSERVED: every one is a register that changed when Linux
 * on this same device turned its sine generator on, captured by diffing the
 * whole PMIC across the transition TWICE and keeping only registers that
 * changed to the SAME value both times. That filter matters -- a single-run
 * diff also contained 0x248a and 0x2492, which did not reproduce and are
 * therefore an ADC reading or a counter, not configuration. Writing those
 * would have been writing noise into a PMIC.
 *
 * The three outside the 0x22xx-0x24xx audio band (0xd8, 0x7ac, 0x1822) are
 * the least certain rows here: they are almost certainly the clock buffer and
 * an audio LDO, but "almost certainly" is doing real work in that sentence.
 * They are included because the analog half cannot come up without its supply,
 * and they are listed last so the log says plainly if one of them is what
 * wedges the machine. Registers in bands that were demonstrably noisy
 * elsewhere (0x10d8, 0x434) are EXCLUDED even though they passed the
 * two-run filter, because their neighbours all vary run to run and a
 * coincidence at that rate is likelier than a configuration bit.
 *
 * Full derivation, including what could not be measured (the ORDER): see
 * cosmo64/AUDIO-SURVEY.md. */
#if C64_AUDIO && C64_PMIC_WRITE
/* `must` distinguishes a register we are DRIVING from one we merely observed
 * change. A diff cannot tell the two apart -- it sees 0 -> 8 either way -- and
 * the first hardware run made the difference concrete: every row took except
 * 0x22ac, which read back 0 after being written 8. A control bit that refuses
 * a write is a broken bring-up; a STATUS bit that refuses one was never a
 * control bit at all, and its value under Linux was a consequence of the codec
 * coming up rather than a cause of it. Rows marked must=0 are still written
 * (harmless if they turn out to be control after all) but do not fail the set. */
static const struct { c64_u32 addr; c64_u32 val; int must; const char *name; } k_aud[] = {
    /* the codec's digital half */
    { 0x220Cu, 0x0000u, 1, "AUD 220c" },
    { 0x2240u, 0x0000u, 1, "AUD 2240" },
    { 0x2288u, 0x0001u, 1, "AUD 2288" },
    { 0x228Au, 0x0001u, 1, "AUD 228a" },
    { 0x2292u, 0x002Au, 1, "AUD 2292" },
    { 0x2296u, 0xCBA1u, 1, "AUD 2296" },
    { 0x229Au, 0x000Bu, 1, "AUD 229a" },
    { 0x22ACu, 0x0008u, 0, "AUD 22ac" },
    { 0x22D6u, 0x002Au, 1, "AUD 22d6" },
    { 0x2394u, 0x0061u, 1, "AUD 2394" },
    /* the analog half: DAC, PGA, the amps */
    { 0x2408u, 0x3AFFu, 1, "AUD 2408" },
    { 0x240Au, 0x3F03u, 1, "AUD 240a" },
    { 0x240Cu, 0xC033u, 1, "AUD 240c" },
    { 0x2410u, 0x0040u, 1, "AUD 2410" },
    { 0x241Au, 0xF201u, 1, "AUD 241a" },
    { 0x2420u, 0x0055u, 1, "AUD 2420" },
    { 0x2422u, 0x0001u, 1, "AUD 2422" },
    { 0x2424u, 0x1055u, 1, "AUD 2424" },
    { 0x2426u, 0x0001u, 1, "AUD 2426" },
    { 0x248Cu, 0x050Au, 1, "AUD 248c" },
    /* supply and clock: the least certain rows, hence last */
    { 0x00D8u, 0x0249u, 1, "AUD clkbuf 00d8" },
    { 0x07ACu, 0xA2B5u, 1, "AUD supply 07ac" },
    { 0x1822u, 0x0006u, 1, "AUD 1822" },
};
#define AUD_COUNT (int)(sizeof k_aud / sizeof k_aud[0])
#endif  /* C64_AUDIO && C64_PMIC_WRITE */

static int g_ready;

/* ---- timing -------------------------------------------------------------- */

static c64_u64 deadline_ms(unsigned ms)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    return c64_cnt_now() + (f / 1000ull) * ms;
}

#if C64_PMIC_WRITE
static void spin_us(c64_u32 us)
{
    c64_u64 f = c64_cnt_freq();
    if (!f)
        f = 13000000ull;
    c64_u64 until = c64_cnt_now() + (f / 1000000ull) * us + 1;
    while (c64_cnt_now() < until)
        ;
}
#endif

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

#if C64_PMIC_WRITE
/* The only way to write, and in a read-only build it is not compiled at all
 * -- neither is the wrapper call it would make. That is deliberate and it is
 * the difference between a build that declines to write and a build that
 * cannot: there is no runtime state, no flag and no mistake that turns this
 * one into the other. `idx` indexes the whitelist above; there is no address
 * parameter, so there is no way to reach a rail this port has not thought
 * about. */
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
#endif  /* C64_PMIC_WRITE */

#if C64_AUDIO && C64_PMIC_WRITE
/* Apply the audio set in table order, reading each row back. The read-back is
 * not ceremony for the same reason it is not ceremony in pmic_rmw(): this is a
 * serial bus to another chip, and a write returning success only means the
 * wrapper accepted the command.
 *
 * A row that does not read back is logged and the whole thing FAILS rather
 * than pressing on: half a codec is not a quieter codec, it is an unknown
 * analog state, and the caller's next act would be to enable a tone into it.
 * Returns 0 if every row took, -1 otherwise. */
int c64_pmic_audio_apply(void)
{
    int i, bad = 0;

    if (!g_ready) {
        c64_log("pmic: audio set skipped -- the wrapper never came up\n");
        return -1;
    }
    for (i = 0; i < AUD_COUNT; i++) {
        c64_u32 back = 0;
        if (wacs2(1, k_aud[i].addr, k_aud[i].val, 0) < 0) {
            c64_logf("pmic: %s (%04x) write FAILED\n", k_aud[i].name, k_aud[i].addr);
            bad++;
            continue;
        }
        if (c64_pmic_read(k_aud[i].addr, &back) < 0) {
            c64_logf("pmic: %s (%04x) read-back FAILED\n", k_aud[i].name, k_aud[i].addr);
            bad++;
            continue;
        }
        if (back != k_aud[i].val) {
            c64_logf("pmic: %s (%04x) wanted %04x, reads %04x%s\n",
                     k_aud[i].name, k_aud[i].addr, k_aud[i].val, back,
                     k_aud[i].must ? "" : "  (status, not a control bit)");
            if (k_aud[i].must)
                bad++;
        }
    }
    c64_logf("pmic: audio set applied -- %d of %d rows took\n",
             AUD_COUNT - bad, AUD_COUNT);
    return bad ? -1 : 0;
}
#endif  /* C64_AUDIO && C64_PMIC_WRITE */

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

/* Selector -> millivolts, or 0 for a value that is not a legal selector at
 * all. The "not legal" answer is the useful one: these fields have three or
 * four permitted codes out of eight or sixteen, so a field read from the
 * WRONG address is much more likely to decode as nonsense than as a valid
 * voltage. It is a weak checksum on the address map, and it is free. */
static int vmch_mv(c64_u32 sel)
{
    return sel == 2u ? 2900 : sel == 3u ? 3000 : sel == 5u ? 3300 : 0;
}

static int vmc_mv(c64_u32 sel)
{
    return sel == 4u ? 1800 : sel == 10u ? 2900
         : sel == 11u ? 3000 : sel == 13u ? 3300 : 0;
}

void c64_pmic_sd_rails_on(void)
{
    c64_u32 ec = 0, ea = 0, hc = 0, ha = 0, cc = 0, ca = 0;
    int emv, hmv, cmv, map_ok;

    if (!g_ready) {
        c64_log("sd: no PMIC -- the card's rails cannot be switched on\n");
        return;
    }

    if (c64_pmic_read(MT6358_LDO_VEMC_CON0, &ec) < 0
        || c64_pmic_read(MT6358_VEMC_ANA_CON0, &ea) < 0
        || c64_pmic_read(MT6358_LDO_VMCH_CON0, &hc) < 0
        || c64_pmic_read(MT6358_VMCH_ANA_CON0, &ha) < 0
        || c64_pmic_read(MT6358_LDO_VMC_CON0, &cc) < 0
        || c64_pmic_read(MT6358_VMC_ANA_CON0, &ca) < 0) {
        c64_log("sd: could not read the rail registers -- writing nothing\n");
        return;
    }

    emv = vmch_mv((ea >> 8) & 7u);       /* VEMC shares VMCH's selector table */
    hmv = vmch_mv((ha >> 8) & 7u);
    cmv = vmc_mv((ca >> 8) & 0xFu);

    c64_logf("sd: VEMC en=%d sel=%d (%d mV) CON0=%04x ANA=%04x <- the eMMC's rail\n",
             (int)(ec & 1u), (int)((ea >> 8) & 7u), emv, ec, ea);
    c64_logf("sd: VMCH en=%d sel=%d (%d mV) CON0=%04x ANA=%04x <- the card's supply\n",
             (int)(hc & 1u), (int)((ha >> 8) & 7u), hmv, hc, ha);
    c64_logf("sd: VMC  en=%d sel=%d (%d mV) CON0=%04x ANA=%04x <- the card's I/O\n",
             (int)(cc & 1u), (int)((ca >> 8) & 0xFu), cmv, cc, ca);

    /* THE CONTROL. The eMMC is working -- msdc.c has already read the GPT off
     * it this boot -- so its rail cannot be off, and it cannot be sitting at
     * an illegal voltage. If the map says otherwise, the map is wrong, and
     * the only safe thing to do with a wrong PMIC address map is nothing. */
    map_ok = (ec & 1u) && emv;
    if (!map_ok) {
        c64_logf("sd: MAP NOT CONFIRMED -- VEMC reads %s at %d mV, but the "
                 "eMMC is demonstrably working, so these addresses are not "
                 "where this driver thinks they are. WRITING NOTHING.\n",
                 (ec & 1u) ? "enabled" : "DISABLED", emv);
        return;
    }
    c64_logf("sd: MAP CONFIRMED -- VEMC, the rail the working eMMC runs on, "
             "reads enabled at %d mV, so the same map's VMCH and VMC readings "
             "above are believable.\n", emv);

    /* A SECOND GATE, on the voltages rather than the addresses. Enabling a
     * rail is only safe if the selector it is already carrying is one this
     * driver would have chosen. The measured state is 3.0 V on both, which is
     * inside default speed's 2.7-3.6 V window -- but a preloader that left
     * VMC at 1.8 V (Linux's UHS value) would make the card's I/O rail a volt
     * below its supply, and switching that on is not a thing to do because a
     * loop happened to reach it. */
    if (hmv < 2900 || cmv < 2900) {
        c64_logf("sd: rails are selected at VMCH %d mV / VMC %d mV, and this "
                 "driver only enables them at 2.9 V or above -- REFUSING. "
                 "Setting a selector is a separate decision from setting an "
                 "enable bit, and it has not been made.\n", hmv, cmv);
        return;
    }

#if !C64_PMIC_WRITE
    c64_logf("sd: read-only build -- would now set VMCH_CON0 and VMC_CON0 bit "
             "0, two single-bit writes, leaving both voltages at the %d/%d mV "
             "the boot already selected. Rebuild with PMIC_WRITE=1 to arm.\n",
             hmv, cmv);
    return;
#else
    /* Two bits. No voltage is touched: the selectors were read above and both
     * are already where this driver wants them, so pmic_rmw finds nothing to
     * change and issues no write for them at all.
     *
     * Supply before I/O. A card whose I/O rail comes up while its VDD is
     * still dark has its signal pins driven through their protection diodes,
     * which is a way to power a chip that no datasheet endorses. */
    if (pmic_rmw(WR_VMCH_EN, 0x0001u, 1) < 0) {
        c64_log("sd: could not bring VMCH (the card's supply) up\n");
        return;
    }
    spin_us(1000);
    if (pmic_rmw(WR_VMC_EN, 0x0001u, 1) < 0) {
        c64_log("sd: could not bring VMC (the card's I/O rail) up\n");
        return;
    }
    /* The device tree asks for 60 us of ramp. Give it two orders of magnitude
     * more and call it free: this happens once, at boot, and the SD
     * specification wants a settled supply before the first command anyway. */
    spin_us(10000);
    c64_logf("sd: VMCH and VMC are on at %d and %d mV\n", hmv, cmv);
#endif
}
