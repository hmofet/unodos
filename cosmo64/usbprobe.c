/* cosmo64/usbprobe.c -- bring the USB host controller far enough up to say
 * whether a device is plugged into it.
 *
 *   ./build.sh usb   ->  build/pc64arm-boot.img
 *
 * WHAT v1 ANSWERED (hardware, 2026-09-01). The whole point was to find out
 * which of M3a's two worlds this is: "LK left it live, adopt it" or "LK never
 * touched it, bring it up". The answer is neither, and it is better than the
 * second one:
 *
 *   infra_usb (STA2 bit1) = 0 -> the SSUSB clock is running
 *   HW_ID=20160812                     <- the IP answers
 *   PW_CTRL0=10411021 (SW_RST 1)       <- but it is held in reset
 *   CTRL1=00000001 (HOST_PDN 1)        <- and the host is powered down
 *   MAC_CAP=00000101 XHCI_CAP=00000100 <- 1 u2 port, 0 u3 ports
 *   CAPLENGTH=00                       <- zeros, exactly as reset implies
 *
 * So the clock tree is already ours for free (every INFRA_PDN_STA register
 * read 0: LK gates nothing), the IP is present and answering, and what stands
 * between us and a working port is a reset it is being held in. v1 deliberately
 * did not deassert that reset, because the first job of a probe is to record
 * the state it was handed. It has been recorded; v2 deasserts it.
 *
 * WHAT v2 DOES. The smallest bring-up that can make PORTSC mean something,
 * in the order the hardware requires:
 *
 *   1. pulse IP_SW_RST (assert, 1 us, deassert) and keep the device IP
 *      powered down, which is what lets the host side own the port;
 *   2. clear IP_HOST_PDN, then per port clear PDN and DIS and select HOST
 *      role, re-reading XHCI_CAP after the reset rather than trusting the
 *      count read while the IP was still held;
 *   3. wait for the STS1 clock-stable bits;
 *   4. initialise the U2 T-PHY for host: stop forcing suspend, enable the OTG
 *      VBUS comparator, and tell it VBUS is valid and we are the A-device
 *      (DTM1: set RG_VBUSVALID and RG_AVALID, clear RG_SESSEND). That last
 *      write is the whole of "be a host" as far as the PHY is concerned;
 *   5. power the root port (PORTSC.PP) if it did not come up powered, and
 *      read PORTSC before and after a debounce delay.
 *
 * ONLY THE USB2 PATH. XHCI_CAP reports one u2 port and zero u3 ports, and the
 * hub on this device is a USB 2.0 hub, so there is nothing a SuperSpeed
 * bring-up would reach. The U3 PHY is left alone.
 *
 * IT STILL CARRIES NO I2C. The expensive part of USB on a phone is the Type-C
 * role and VBUS, and the evidence that retires all of it is PORTSC: if the
 * controller reports a connection then role, VBUS and cabling are already good
 * enough to enumerate, and the FUSB301 on bus 3 (0x1100f000, address 0x25)
 * never needs to be written. The hub plugged in here reports itself
 * SELF-POWERED under Linux (bmAttributes 0xe0, bMaxPower 0), so nothing
 * downstream is asking the Cosmo for 5 V either. Ask the cheap question first.
 *
 * SAFETY. Reads of a gated block hang with no fault, so the gate is read (and
 * cleared if needed) before anything else, and the log is flushed before each
 * phase with the register named in advance: a wedge leaves its own cause as
 * the last durable line. Every wait is bounded.
 *
 * FACT SOURCES (facts only -- register offsets and bit positions, no code):
 *   - LK's mt6771 mt_ssusb_sifslv_ippc.h for the IPPC map and its base
 *     (USB3_BASE + 0x3E00), and mt_usb.h for the PHY block bases inside sif2;
 *     both proprietary to MediaTek;
 *   - the Gemian 4.4 kernel for the IPPC struct layout, the per-port control
 *     bits and the STS1 stability mask (xhci-mtk), the U2 T-PHY register map
 *     and its host-mode sequence (phy-mt65xx-usb3.c), and the infra gate
 *     (clk-mt6771.c: infra_usb = INFRA2 bit 1); GPL;
 *   - this device's own DTB for the three bases: ssusb_base 0x11200000,
 *     ssusb_ippc 0x11203e00, ssusb_sif2 0x11f40000;
 *   - the xHCI specification for everything at ssusb_base (CAPLENGTH,
 *     HCSPARAMS1, USBCMD/USBSTS, PORTSC), which is public and vendor-neutral.
 * Neither of the first two may be copied into UnoDOS. The code below is this
 * project's; see the same note at the head of msdc.c.
 *
 * ON SCREEN, so a wedge is visible without a reboot. Each phase paints a
 * 32x32 block as it COMPLETES, in this order:
 *   blue   gates read         cyan   clock ungated / already running
 *   yellow IPPC read          orange xHCI read as handed over
 *   white  reset released + host and ports enabled
 *   purple T-PHY put in host mode
 *   green  finished, log is complete
 * A red block with hex rows is the fault handler: the probe took an exception.
 *
 * THEY RUN TOP TO BOTTOM DOWN THE RIGHT-HAND EDGE, not left to right. The
 * beacons are painted in the panel's OWN portrait frame (1080x2160 at the
 * measured base), deliberately: they have to work before the framebuffer is
 * adopted and before anything is rotated, which is the whole point of having
 * them. The shell's 270-degree rotation is what makes the desktop land in
 * landscape, and a run along the panel's top edge lands along one vertical
 * edge of that view. Nothing is wrong when they appear vertical; it is the
 * absence of the rotation, not a bug in it. (Observed on hardware
 * 2026-09-01.)
 *
 * Then: hold power, boot trixie, ./readlog.sh.
 */

#include "cosmo64.h"

#define R8(a)  (*(volatile c64_u8  *)(c64_u64)(a))
#define R16(a) (*(volatile unsigned short *)(c64_u64)(a))
#define R32(a) (*(volatile c64_u32 *)(c64_u64)(a))

/* ---- infracfg: the clock gate (clk-mt6771.c) ------------------------------ */
#define INFRACFG      0x10001000u
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

#define CTRL0_IP_SW_RST (1u << 0)
#define CTRL1_HOST_PDN  (1u << 0)
#define CTRL2_DEV_PDN   (1u << 0)
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

/* ---- the U2 T-PHY (phy-mt65xx-usb3.c; COM block at sif2 + 0x300) ---------- */
#define U2COM          (SSUSB_SIF2 + 0x300)
#define PHY_ACR5       (U2COM + 0x14)
#define PHY_ACR6       (U2COM + 0x18)
#define PHY_DTM0       (U2COM + 0x68)
#define PHY_DTM1       (U2COM + 0x6C)

#define PA5_HSTX_SRCTRL     (7u << 12)
#define PA5_HSTX_SRCTRL_4   (4u << 12)
#define PA6_OTG_VBUSCMP_EN  (1u << 20)

#define P2C_FORCE_DATAIN       (1u << 23)
#define P2C_FORCE_DM_PULLDOWN  (1u << 21)
#define P2C_FORCE_DP_PULLDOWN  (1u << 20)
#define P2C_FORCE_XCVRSEL      (1u << 19)
#define P2C_FORCE_SUSPENDM     (1u << 18)
#define P2C_FORCE_TERMSEL      (1u << 17)
#define P2C_RG_DATAIN          (0xFu << 10)
#define P2C_RG_DMPULLDOWN      (1u << 7)
#define P2C_RG_DPPULLDOWN      (1u << 6)
#define P2C_RG_XCVRSEL         (3u << 4)
#define P2C_RG_TERMSEL         (1u << 2)
#define P2C_DTM0_PART_MASK (P2C_FORCE_DATAIN | P2C_FORCE_DM_PULLDOWN | \
                            P2C_FORCE_DP_PULLDOWN | P2C_FORCE_XCVRSEL | \
                            P2C_FORCE_TERMSEL | P2C_RG_DMPULLDOWN | \
                            P2C_RG_DPPULLDOWN | P2C_RG_TERMSEL)

#define P2C_RG_VBUSVALID (1u << 5)
#define P2C_RG_SESSEND   (1u << 4)
#define P2C_RG_AVALID    (1u << 2)

/* ---- PORTSC (xHCI spec) --------------------------------------------------- */
#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)          /* write-1-to-DISABLE: never write 1 */
#define PORTSC_PP   (1u << 9)
#define PORTSC_RW1C 0x00FE0000u        /* the change bits, all write-1-clear */

/* ---- beacons -------------------------------------------------------------- */
#define BCN_GATES  320
#define BCN_UNGATE 368
#define BCN_IPPC   416
#define BCN_XHCI   464
#define BCN_HOST   512
#define BCN_PHY    560
#define BCN_DONE   608

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

/* ---- the IP port controller ----------------------------------------------- */
static void ippc_read(const char *when)
{
    c64_u32 id, sub, c0, c1, c2, c3, s1, s2, mac, xcap;

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

    c64_logf("usbprobe [%s]: HW_ID=%08x HW_SUB_ID=%08x\n", when, id, sub);
    c64_logf("usbprobe [%s]: PW_CTRL0=%08x (SW_RST %d) CTRL1=%08x "
             "(HOST_PDN %d)\n", when, c0, (int)(c0 & 1), c1, (int)(c1 & 1));
    c64_logf("usbprobe [%s]: PW_CTRL2=%08x (DEV_PDN %d) CTRL3=%08x\n",
             when, c2, (int)(c2 & 1), c3);
    c64_logf("usbprobe [%s]: PW_STS1=%08x PW_STS2=%08x (STS1 ready mask %s)\n",
             when, s1, s2, (s1 & STS1_READY) == STS1_READY ? "MET" : "not met");
    c64_logf("usbprobe [%s]: MAC_CAP=%08x XHCI_CAP=%08x -> %d u3 port(s), "
             "%d u2 port(s)\n", when, mac, xcap, (int)(xcap & 0xFF),
             (int)((xcap >> 8) & 0xFF));

    if (id == 0xFFFFFFFFu)
        c64_log("usbprobe: HW_ID reads all-ones -- nothing is answering at "
                "this address\n");
}

/* ---- the xHCI itself (public spec) ---------------------------------------- */
/* CAPLENGTH is the byte offset from ssusb_base to the operational registers;
 * the port registers are a 0x10-byte block each starting at op+0x400. Returns
 * the operational base, or 0 if the controller is not answering. */
static c64_u32 xhci_read(const char *when)
{
    c64_u32 caplen, hciver, hcs1, hcc1, op, cmd, sts;
    int ports, i, connected = 0;

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
        return 0;
    }
    op = SSUSB_BASE + caplen;
    cmd = R32(op + 0x00);
    sts = R32(op + 0x04);
    c64_logf("usbprobe [%s]: USBCMD=%08x (RUN %d) USBSTS=%08x (HCHalted %d, "
             "CNR %d)\n", when, cmd, (int)(cmd & 1), sts, (int)(sts & 1),
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
        if (psc & PORTSC_CCS)
            connected = i + 1;
    }
    /* THE LINE THAT DECIDES THE MILESTONE. A connected port means the Type-C
     * role, VBUS and the PHY are all already good enough to see a device, and
     * the whole Type-C lane is work that does not exist. */
    if (connected)
        c64_logf("usbprobe [%s]: *** A DEVICE IS CONNECTED on port %d *** -- "
                 "role, VBUS and PHY are good enough to enumerate\n",
                 when, connected);
    else
        c64_logf("usbprobe [%s]: no port reports a connection\n", when);
    return op;
}

/* ---- release the reset and power the host up ------------------------------ */
static void host_enable(void)
{
    c64_u32 xcap, v, s1;
    int u3, u2, i;

    c64_log("usbprobe: pulsing IP_SW_RST, then powering the host up\n");
    c64_log_flush();

    /* The IP arrives held in reset (v1's finding). Pulse rather than merely
     * release, so the block starts from a defined state whichever way it was
     * left: assert, settle, deassert. */
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL0);
    R32(SSUSB_IPPC + IPPC_PW_CTRL0) = v | CTRL0_IP_SW_RST;
    __asm__ volatile("dsb sy" ::: "memory");
    spin_us(10);
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL0);
    R32(SSUSB_IPPC + IPPC_PW_CTRL0) = v & ~CTRL0_IP_SW_RST;
    __asm__ volatile("dsb sy" ::: "memory");
    spin_us(10);
    c64_logf("usbprobe: PW_CTRL0 now %08x (SW_RST %d)\n",
             R32(SSUSB_IPPC + IPPC_PW_CTRL0),
             (int)(R32(SSUSB_IPPC + IPPC_PW_CTRL0) & 1));

    /* Keep the device IP powered down. It powers on by default, and leaving
     * it on is what stops the host side owning the port. */
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL2);
    R32(SSUSB_IPPC + IPPC_PW_CTRL2) = v | CTRL2_DEV_PDN;

    /* power on the host IP */
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL1);
    R32(SSUSB_IPPC + IPPC_PW_CTRL1) = v & ~CTRL1_HOST_PDN;

    /* Re-read the port counts AFTER the reset: the ones v1 logged were read
     * while the IP was still held, and a capability register read in that
     * state is not evidence of anything. */
    xcap = R32(SSUSB_IPPC + IPPC_XHCI_CAP);
    u3 = (int)(xcap & 0xFF);
    u2 = (int)((xcap >> 8) & 0xFF);
    c64_logf("usbprobe: XHCI_CAP after reset = %08x -> %d u3, %d u2\n",
             xcap, u3, u2);
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
}

/* ---- put the U2 PHY into host mode ---------------------------------------- */
/* The four writes that matter, in the vendor driver's order. The last one is
 * the whole of "be a host" as far as the analogue side is concerned: with
 * SESSEND clear and VBUSVALID and AVALID set, the PHY believes it is the
 * A-device on a powered bus, which is what makes it drive the pull-downs and
 * see a device's pull-up. */
static void phy_host_init(void)
{
    c64_u32 v;

    announce("the U2 T-PHY COM block at 0x11f40300");
    c64_logf("usbprobe: PHY before: ACR5=%08x ACR6=%08x DTM0=%08x DTM1=%08x\n",
             R32(PHY_ACR5), R32(PHY_ACR6), R32(PHY_DTM0), R32(PHY_DTM1));

    /* stop forcing suspend, and stop forcing the transceiver's pull-downs:
     * let the port's own state machine drive them */
    v = R32(PHY_DTM0);
    v &= ~(P2C_FORCE_SUSPENDM | P2C_RG_XCVRSEL);
    v &= ~(P2C_RG_DATAIN | P2C_DTM0_PART_MASK);
    R32(PHY_DTM0) = v;

    /* OTG VBUS comparator on */
    v = R32(PHY_ACR6);
    R32(PHY_ACR6) = v | PA6_OTG_VBUSCMP_EN;

    /* the host declaration */
    v = R32(PHY_DTM1);
    v |= P2C_RG_VBUSVALID | P2C_RG_AVALID;
    v &= ~P2C_RG_SESSEND;
    R32(PHY_DTM1) = v;

    /* USB 2.0 slew-rate calibration, the vendor's default step */
    v = R32(PHY_ACR5);
    R32(PHY_ACR5) = (v & ~PA5_HSTX_SRCTRL) | PA5_HSTX_SRCTRL_4;
    __asm__ volatile("dsb sy" ::: "memory");

    c64_logf("usbprobe: PHY after:  ACR5=%08x ACR6=%08x DTM0=%08x DTM1=%08x\n",
             R32(PHY_ACR5), R32(PHY_ACR6), R32(PHY_DTM0), R32(PHY_DTM1));
}

/* ---- make sure the root port is powered ----------------------------------- */
/* PORTSC is full of write-1-to-clear change bits, and PED is write-1-to-
 * DISABLE, so a naive read-modify-write of this register disables the port it
 * was meant to power. Mask both off before writing anything back. */
static void port_power(c64_u32 op, int ports)
{
    int i;
    for (i = 0; i < ports; i++) {
        c64_u32 a = op + 0x400 + 0x10u * (c64_u32)i;
        c64_u32 psc = R32(a);
        if (psc & PORTSC_PP)
            continue;
        c64_logf("usbprobe: port %d came up unpowered (PORTSC=%08x); "
                 "setting PP\n", i + 1, psc);
        R32(a) = (psc & ~(PORTSC_RW1C | PORTSC_PED)) | PORTSC_PP;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

/* ---- boot ---------------------------------------------------------------- */
void c_main(void *dtb)
{
    c64_u32 op;

    c64_beacon(224, 0xFFFF00FFu);          /* MAGENTA: C reached */
    c64_log_survey();
    c64_log_init();
    c64_log("usbprobe payload: bring the USB host up far enough to see a "
            "device\n");
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
        c64_logf("usbprobe: INFRA_PDN_STA2 now %08x\n", R32(INFRA_STA2));
    } else {
        c64_log("usbprobe: the SSUSB clock was already running at handover -- "
                "LK gates nothing\n");
    }
    c64_beacon(BCN_UNGATE, 0xFF00FFFFu);           /* CYAN */
    c64_log_flush();

    announce("the IPPC block at 0x11203e00");
    ippc_read("as handed over");
    c64_beacon(BCN_IPPC, 0xFFFFFF00u);             /* YELLOW */
    c64_log_flush();

    announce("the xHCI capability registers at 0x11200000");
    xhci_read("as handed over");
    c64_beacon(BCN_XHCI, 0xFFFF8000u);             /* ORANGE */
    c64_log_flush();

    host_enable();
    c64_beacon(BCN_HOST, 0xFFFFFFFFu);             /* WHITE */
    ippc_read("after host enable");
    c64_log_flush();

    phy_host_init();
    c64_beacon(BCN_PHY, 0xFF8000FFu);              /* PURPLE */
    c64_log_flush();

    op = xhci_read("after reset + PHY");
    if (op) {
        c64_u32 hcs1 = R32(SSUSB_BASE + 0x04);
        int ports = (int)((hcs1 >> 24) & 0xFF);
        if (ports > 16)
            ports = 16;
        port_power(op, ports);
        /* A port takes time to see a device even when everything else is
         * right: the hub has to be detected and debounced. Give it a beat
         * rather than concluding "nothing connected" from an instant sample. */
        c64_log("usbprobe: waiting 500 ms for connect debounce...\n");
        c64_log_flush();
        spin_us(500000);
        xhci_read("after debounce");
    }

    c64_log("usbprobe: done. Hold power, boot trixie, ./readlog.sh\n");
    c64_log_flush();
    c64_beacon(BCN_DONE, 0xFF00FF00u);             /* GREEN: complete */
    for (;;)
        __asm__ volatile("wfe");
}
