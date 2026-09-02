/* cosmo64/usbprobe.c -- what state does LK leave the USB host controller in?
 *
 *   ./build.sh usb   ->  build/pc64arm-boot.img
 *
 * M3a cost a day because the answer to that question was "fully live, adopt
 * it". M3b's SD-card sibling will cost a week because for MSDC1 the answer is
 * "untouched, bring it up yourself". The difference between those two worlds
 * is a handful of register reads, and this payload is those reads: it makes no
 * claims, it reports.
 *
 * THE QUESTION, precisely. A USB mouse needs four things to be true, and each
 * one is a different amount of work:
 *
 *   1. the SSUSB clock is ungated                  (one infracfg write)
 *   2. the IP is out of power-down and the ports   (a dozen IPPC writes)
 *      are enabled in HOST role
 *   3. the T-PHY is initialised                    (unknown; possibly large)
 *   4. the Type-C port is in DFP role with VBUS    (I2C to the FUSB301 and
 *      where the device needs it                    the MT6370 charger)
 *
 * Point 4 is normally the expensive one on a phone, and there is reason to
 * hope it is already free here: the hub currently plugged into this device
 * reports itself SELF-POWERED (bmAttributes 0xe0, bMaxPower 0) under Linux, so
 * nothing downstream is asking the Cosmo for 5 V. And the decisive evidence
 * for the whole of point 4 is not an I2C register at all -- it is PORTSC. If
 * the controller reports a connection, then role, VBUS and cabling are all
 * already right and none of that work exists. That is why this probe carries
 * no I2C driver: it asks the question that can retire the most work first, and
 * the I2C bus (bus 3 at 0x1100f000, the FUSB301 at 0x25) only gets written if
 * this comes back saying the port sees nothing.
 *
 * WHAT IT WRITES. Phases 1-4 are reads, and reads of a gated block can hang a
 * bus with no fault, so the gate is checked and ungated FIRST and the log is
 * flushed before each phase -- if it wedges, the last line in the log names
 * the exact register that did it. Phase 5 then attempts the SMALLEST bring-up
 * that could make PORTSC meaningful (clear the host power-down and the per-
 * port PDN/DIS, select host role, wait for the clock-stable bits) and
 * re-reads. It deliberately does NOT assert IP_SW_RST: the same instinct that
 * says never MSDC_CFG_RST says do not reset a block before you have recorded
 * the state you were handed. If phase 5 turns out to be insufficient, the
 * reset is the first thing the next iteration adds.
 *
 * FACT SOURCES (facts only -- register offsets and bit positions, no code):
 *   - LK's mt6771 mt_ssusb_sifslv_ippc.h for the IPPC map and IPPC base
 *     (USB3_BASE + 0x3E00), which is proprietary to MediaTek;
 *   - the Gemian 4.4 kernel's xhci-mtk.c/.h for the IPPC struct layout, the
 *     per-port control bits and the STS1 stability mask, and clk-mt6771.c for
 *     the infra gate (infra_usb = INFRA2 bit 1), which is GPL;
 *   - this device's own DTB for the three base addresses:
 *     ssusb_base 0x11200000, ssusb_ippc 0x11203e00, ssusb_sif2 0x11f40000;
 *   - the xHCI specification for everything at ssusb_base (CAPLENGTH,
 *     HCSPARAMS1, USBCMD/USBSTS, PORTSC), which is public and vendor-neutral.
 * Neither of the first two may be copied into UnoDOS. The code below is this
 * project's; see the same note at the head of msdc.c.
 *
 * ON SCREEN, so a wedge is visible without a reboot. Each phase paints a
 * 32x32 block left to right as it COMPLETES:
 *   blue 320   gates read        cyan 368   clock ungated
 *   yellow 416 IPPC read         orange 464 xHCI read
 *   white 512  host enabled      green 560  finished, log is complete
 * A red block with hex rows is the fault handler: the probe took an exception.
 * The screen otherwise stays as LK left it.
 *
 * Then: hold power, boot trixie, ./readlog.sh.
 */

#include "cosmo64.h"

#define R8(a)  (*(volatile c64_u8  *)(c64_u64)(a))
#define R16(a) (*(volatile unsigned short *)(c64_u64)(a))
#define R32(a) (*(volatile c64_u32 *)(c64_u64)(a))

/* ---- infracfg: the clock gate (clk-mt6771.c) ------------------------------ */
#define INFRACFG      0x10001000u
#define INFRA_CLR0    (INFRACFG + 0x84)
#define INFRA_STA0    (INFRACFG + 0x90)
#define INFRA_STA1    (INFRACFG + 0x94)
#define INFRA_CLR2    (INFRACFG + 0xA8)
#define INFRA_STA2    (INFRACFG + 0xAC)
#define INFRA_STA3    (INFRACFG + 0xC8)
#define GATE_USB_BIT   1u              /* infra_usb,   INFRA2 bit 1 */
#define GATE_ICUSB_BIT 8u              /* infra_icusb, INFRA0 bit 8 */

/* ---- SSUSB (device tree: ssusb_base / ssusb_ippc / ssusb_sif2) ------------ */
#define SSUSB_BASE 0x11200000u
#define SSUSB_IPPC 0x11203E00u
#define SSUSB_SIF2 0x11F40000u

/* IPPC offsets (LK's header; layout confirmed by the kernel's struct) */
#define IPPC_PW_CTRL0  0x00            /* bit0 = IP_SW_RST                  */
#define IPPC_PW_CTRL1  0x04            /* bit0 = IP_HOST_PDN                */
#define IPPC_PW_CTRL2  0x08            /* bit0 = IP_DEV_PDN                 */
#define IPPC_PW_CTRL3  0x0C            /* bit0 = IP_PCIE_PDN                */
#define IPPC_PW_STS1   0x10
#define IPPC_PW_STS2   0x14
#define IPPC_MAC_CAP   0x20
#define IPPC_XHCI_CAP  0x24            /* [7:0] u3 ports, [15:8] u2 ports   */
#define IPPC_U3_CTRL(i) (0x30 + 8u * (i))      /* u64 slots, low word used  */
#define IPPC_U2_CTRL(i) (0x50 + 8u * (i))      /* after 4 u3 slots          */
#define IPPC_HW_ID     0xA0
#define IPPC_HW_SUB_ID 0xA4

#define CTRL1_HOST_PDN  (1u << 0)
#define PORT_DIS        (1u << 0)
#define PORT_PDN        (1u << 1)
#define PORT_HOST_SEL   (1u << 2)

/* the clock-stable bits STS1 must show once the host is powered */
#define STS1_SYSPLL_STABLE (1u << 0)
#define STS1_REF_RST       (1u << 8)
#define STS1_SYS125_RST    (1u << 10)
#define STS1_XHCI_RST      (1u << 11)
#define STS1_READY (STS1_SYSPLL_STABLE | STS1_REF_RST | STS1_SYS125_RST | \
                    STS1_XHCI_RST)

/* ---- beacons -------------------------------------------------------------- */
#define BCN_GATES  320
#define BCN_UNGATE 368
#define BCN_IPPC   416
#define BCN_XHCI   464
#define BCN_HOST   512
#define BCN_DONE   560

static void spin_us(c64_u32 us)
{
    c64_u64 hz = c64_cnt_freq();
    if (!hz)
        hz = 13000000ull;
    c64_u64 until = c64_cnt_now() + ((hz / 1000000ull) * us) + 1;
    while (c64_cnt_now() < until)
        ;
}

/* Every phase says what it is about to touch and then flushes, so that a bus
 * hang -- which produces no fault and no further output -- still leaves the
 * name of the register that hung as the last durable line. */
static void announce(const char *what)
{
    c64_logf("usbprobe: about to read %s\n", what);
    c64_log_flush();
}

/* ---- phase 1: is the SSUSB clock even running? ---------------------------- */
/* A set bit in a PDN_STA register means that clock is POWERED DOWN. Reading
 * these is always safe: infracfg is in the always-on domain and our own I2C
 * driver already writes the neighbouring CLR registers every boot. */
static int gates_read(void)
{
    c64_u32 s0 = R32(INFRA_STA0), s1 = R32(INFRA_STA1);
    c64_u32 s2 = R32(INFRA_STA2), s3 = R32(INFRA_STA3);
    int usb_gated = (s2 >> GATE_USB_BIT) & 1;

    c64_logf("usbprobe: INFRA_PDN_STA0=%08x STA1=%08x STA2=%08x STA3=%08x\n",
             s0, s1, s2, s3);
    c64_logf("usbprobe: infra_usb (STA2 bit1) = %d -> the SSUSB clock is %s\n",
             usb_gated, usb_gated ? "GATED" : "running");
    c64_logf("usbprobe: infra_icusb (STA0 bit8) = %d\n",
             (int)((s0 >> GATE_ICUSB_BIT) & 1));
    return usb_gated;
}

/* ---- phase 3: the IP port controller -------------------------------------- */
static void ippc_read(void)
{
    c64_u32 id, sub, c0, c1, c2, c3, s1, s2, mac, xcap;

    announce("the IPPC block at 0x11203e00");
    id  = R32(SSUSB_IPPC + IPPC_HW_ID);
    sub = R32(SSUSB_IPPC + IPPC_HW_SUB_ID);
    c0  = R32(SSUSB_IPPC + IPPC_PW_CTRL0);
    c1  = R32(SSUSB_IPPC + IPPC_PW_CTRL1);
    c2  = R32(SSUSB_IPPC + IPPC_PW_CTRL2);
    c3  = R32(SSUSB_IPPC + IPPC_PW_CTRL3);
    s1  = R32(SSUSB_IPPC + IPPC_PW_STS1);
    s2  = R32(SSUSB_IPPC + IPPC_PW_STS2);
    mac = R32(SSUSB_IPPC + IPPC_MAC_CAP);
    xcap = R32(SSUSB_IPPC + IPPC_XHCI_CAP);

    c64_logf("usbprobe: HW_ID=%08x HW_SUB_ID=%08x\n", id, sub);
    c64_logf("usbprobe: PW_CTRL0=%08x (SW_RST %d) CTRL1=%08x (HOST_PDN %d)\n",
             c0, (int)(c0 & 1), c1, (int)(c1 & 1));
    c64_logf("usbprobe: PW_CTRL2=%08x (DEV_PDN %d) CTRL3=%08x\n",
             c2, (int)(c2 & 1), c3);
    c64_logf("usbprobe: PW_STS1=%08x PW_STS2=%08x (STS1 ready mask %s)\n",
             s1, s2, (s1 & STS1_READY) == STS1_READY ? "MET" : "not met");
    c64_logf("usbprobe: MAC_CAP=%08x XHCI_CAP=%08x -> %d u3 port(s), "
             "%d u2 port(s)\n", mac, xcap, (int)(xcap & 0xFF),
             (int)((xcap >> 8) & 0xFF));

    /* An all-ones read is what unassigned or unclocked MMIO looks like, and
     * it is worth saying out loud rather than leaving somebody to notice. */
    if (id == 0xFFFFFFFFu)
        c64_log("usbprobe: HW_ID reads all-ones -- nothing is answering at "
                "this address\n");
}

/* ---- phases 4 and 6: the xHCI itself (public spec) ------------------------ */
/* CAPLENGTH is the byte offset from ssusb_base to the operational registers;
 * the port registers are a 0x10-byte block each starting at op+0x400. */
static void xhci_read(const char *when)
{
    c64_u32 caplen, hciver, hcs1, hcc1, op, cmd, sts;
    int ports, i;

    announce("the xHCI capability registers at 0x11200000");
    caplen = R8(SSUSB_BASE + 0x00);
    hciver = R16(SSUSB_BASE + 0x02);
    hcs1   = R32(SSUSB_BASE + 0x04);
    hcc1   = R32(SSUSB_BASE + 0x10);
    c64_logf("usbprobe [%s]: CAPLENGTH=%02x HCIVERSION=%04x HCSPARAMS1=%08x "
             "HCCPARAMS1=%08x\n", when, caplen, (c64_u32)hciver, hcs1, hcc1);

    if (caplen < 0x20 || caplen > 0x80) {
        c64_logf("usbprobe [%s]: CAPLENGTH %02x is not plausible -- the "
                 "controller is not answering; skipping the operational "
                 "registers\n", when, caplen);
        return;
    }
    op = SSUSB_BASE + caplen;
    cmd = R32(op + 0x00);
    sts = R32(op + 0x04);
    c64_logf("usbprobe [%s]: USBCMD=%08x (RUN %d) USBSTS=%08x (HCHalted %d, "
             "CNR %d)\n", when, cmd, (int)(cmd & 1), sts, (int)((sts >> 0) & 1),
             (int)((sts >> 11) & 1));

    ports = (int)((hcs1 >> 24) & 0xFF);
    if (ports > 16)
        ports = 16;
    c64_logf("usbprobe [%s]: HCSPARAMS1 says %d port(s)\n", when, ports);
    for (i = 0; i < ports; i++) {
        c64_u32 psc = R32(op + 0x400 + 0x10u * (c64_u32)i);
        c64_logf("  port %d: PORTSC=%08x CCS=%d PED=%d PP=%d speed=%d\n",
                 i + 1, psc, (int)(psc & 1), (int)((psc >> 1) & 1),
                 (int)((psc >> 9) & 1), (int)((psc >> 10) & 0xF));
    }
    /* THE LINE THAT DECIDES THE MILESTONE. A connected port means the Type-C
     * role, VBUS and the PHY are all already good enough to see a device, and
     * the whole of point 4 in this file's header is work that does not exist. */
    for (i = 0; i < ports; i++)
        if (R32(op + 0x400 + 0x10u * (c64_u32)i) & 1) {
            c64_logf("usbprobe [%s]: *** A DEVICE IS CONNECTED on port %d ***"
                     " -- role, VBUS and PHY are already good enough to "
                     "enumerate\n", when, i + 1);
            return;
        }
    c64_logf("usbprobe [%s]: no port reports a connection\n", when);
}

/* ---- phase 5: the smallest bring-up that makes PORTSC mean anything ------- */
static void host_enable(void)
{
    c64_u32 xcap, v, s1;
    int u3, u2, i;

    c64_log("usbprobe: enabling the host IP (no IP_SW_RST -- see the header)\n");
    c64_log_flush();

    v = R32(SSUSB_IPPC + IPPC_PW_CTRL1);
    R32(SSUSB_IPPC + IPPC_PW_CTRL1) = v & ~CTRL1_HOST_PDN;

    xcap = R32(SSUSB_IPPC + IPPC_XHCI_CAP);
    u3 = (int)(xcap & 0xFF);
    u2 = (int)((xcap >> 8) & 0xFF);
    if (u3 > 4) u3 = 4;                     /* the IPPC has 4 u3 slots */
    if (u2 > 5) u2 = 5;                     /* ...and 5 u2 slots       */

    for (i = 0; i < u3; i++) {
        v = R32(SSUSB_IPPC + IPPC_U3_CTRL(i));
        R32(SSUSB_IPPC + IPPC_U3_CTRL(i)) =
            (v & ~(PORT_PDN | PORT_DIS)) | PORT_HOST_SEL;
    }
    for (i = 0; i < u2; i++) {
        v = R32(SSUSB_IPPC + IPPC_U2_CTRL(i));
        R32(SSUSB_IPPC + IPPC_U2_CTRL(i)) =
            (v & ~(PORT_PDN | PORT_DIS)) | PORT_HOST_SEL;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    /* Bounded, because an unstable clock here must report rather than hang. */
    for (i = 0; i < 200; i++) {
        s1 = R32(SSUSB_IPPC + IPPC_PW_STS1);
        if ((s1 & STS1_READY) == STS1_READY)
            break;
        spin_us(100);
    }
    s1 = R32(SSUSB_IPPC + IPPC_PW_STS1);
    c64_logf("usbprobe: after enable, PW_STS1=%08x -- clocks %s (waited %d "
             "us)\n", s1,
             (s1 & STS1_READY) == STS1_READY ? "STABLE" : "NOT stable",
             i * 100);
    c64_logf("usbprobe: PW_CTRL1=%08x u3_ctrl_p0=%08x u2_ctrl_p0=%08x\n",
             R32(SSUSB_IPPC + IPPC_PW_CTRL1),
             R32(SSUSB_IPPC + IPPC_U3_CTRL(0)),
             R32(SSUSB_IPPC + IPPC_U2_CTRL(0)));

    /* A port takes time to see a device even when everything is right: the
     * hub has to be detected and debounced. Give it a beat before re-reading
     * rather than concluding "nothing connected" from an instant sample. */
    spin_us(300000);
}

/* ---- phase 7: the T-PHY, last because it is the least certain ------------- */
static void tphy_read(void)
{
    announce("the T-PHY at 0x11f40000 (U2 COM block at +0x300)");
    c64_logf("usbprobe: sif2+0x300 %08x %08x %08x %08x\n",
             R32(SSUSB_SIF2 + 0x300), R32(SSUSB_SIF2 + 0x304),
             R32(SSUSB_SIF2 + 0x308), R32(SSUSB_SIF2 + 0x30C));
    c64_logf("usbprobe: sif2+0x700 (SPLLC) %08x  sif2+0x900 (U3PHYD) %08x\n",
             R32(SSUSB_SIF2 + 0x700), R32(SSUSB_SIF2 + 0x900));
}

/* ---- boot ---------------------------------------------------------------- */
void c_main(void *dtb)
{
    c64_beacon(224, 0xFFFF00FFu);          /* MAGENTA: C reached */
    c64_log_survey();
    c64_log_init();
    c64_log("usbprobe payload: what did LK leave the USB host in?\n");
    mmu_init();
    c64_log_survey_report();

    c64_u32 ppitch;
    c64_u64 raw = c64_fb_adopt(dtb, &ppitch);      /* its vram clear wipes
                                                    * the boot beacons */
    c64_logf("usbprobe: panel %016x pitch %d\n", raw, (int)ppitch);

    /* Storage first: everything below is written to the eMMC log, and the log
     * is the entire output of this payload. */
    c64_blk_init();
    c64_log_flush();

    int gated = gates_read();
    c64_beacon(BCN_GATES, 0xFF0000FFu);            /* BLUE */
    c64_log_flush();

    if (gated) {
        c64_log("usbprobe: ungating infra_usb (INFRA_PDN_CLR2 bit 1)\n");
        c64_log_flush();
        R32(INFRA_CLR2) = 1u << GATE_USB_BIT;
        __asm__ volatile("dsb sy" ::: "memory");
        spin_us(100);
        c64_logf("usbprobe: INFRA_PDN_STA2 now %08x -- infra_usb %s\n",
                 R32(INFRA_STA2),
                 ((R32(INFRA_STA2) >> GATE_USB_BIT) & 1) ? "STILL GATED"
                                                         : "running");
    } else {
        c64_log("usbprobe: the SSUSB clock was already running at handover -- "
                "LK left something of the USB block alive\n");
    }
    c64_beacon(BCN_UNGATE, 0xFF00FFFFu);           /* CYAN */
    c64_log_flush();

    ippc_read();
    c64_beacon(BCN_IPPC, 0xFFFFFF00u);             /* YELLOW */
    c64_log_flush();

    xhci_read("as handed over");
    c64_beacon(BCN_XHCI, 0xFFFF8000u);             /* ORANGE */
    c64_log_flush();

    host_enable();
    c64_beacon(BCN_HOST, 0xFFFFFFFFu);             /* WHITE */
    c64_log_flush();

    ippc_read();
    xhci_read("after host enable");
    tphy_read();

    c64_log("usbprobe: done. Hold power, boot trixie, ./readlog.sh\n");
    c64_log_flush();
    c64_beacon(BCN_DONE, 0xFF00FF00u);             /* GREEN: complete */
    for (;;)
        __asm__ volatile("wfe");
}
