/* cosmo64/ssusb.c -- the MediaTek SSUSB host block, brought to the point where
 * a standard xHCI driver can take over (M4).
 *
 * This is the sequence usbprobe.c proved on hardware on 2026-09-01, made
 * reusable and quiet. What it established:
 *
 *   - LK gates nothing: every INFRA_PDN_STA register reads 0 at handover, so
 *     the SSUSB clock is already running and no clock tree work exists;
 *   - the IP answers (HW_ID 20160812) but arrives HELD IN SOFTWARE RESET with
 *     the host side powered down, which is why the xHCI register file reads
 *     as zeros until the reset is released;
 *   - after release + host enable + the U2 T-PHY's host-mode writes, the
 *     controller identifies as xHCI 1.10 with ONE USB2 root port and no
 *     SuperSpeed port, the root port comes up UNPOWERED, and once powered it
 *     reports the attached hub (PORTSC CCS=1) -- meaning the Type-C role and
 *     VBUS are already right and none of that lane has to be touched.
 *
 * So this file does the MediaTek part and nothing else: release the reset,
 * power the host and its port in host role, wait for the clocks, put the PHY
 * in host mode. Everything from CAPLENGTH onwards is the xHCI specification,
 * and that is pc64's xhci.c, unchanged.
 *
 * FACT SOURCES (facts only, no code; see usbprobe.c for the full list): LK's
 * mt6771 mt_ssusb_sifslv_ippc.h and mt_usb.h (proprietary), the Gemian 4.4
 * kernel's xhci-mtk.c/.h, phy-mt65xx-usb3.c and clk-mt6771.c (GPL), and this
 * device's DTB for the base addresses. Nothing is copied from either.
 */

#include "cosmo64.h"

#define R32(a) (*(volatile c64_u32 *)(c64_u64)(a))

/* infracfg (clk-mt6771.c): infra_usb is INFRA2 bit 1 */
#define INFRA_CLR2    0x100010A8u
#define INFRA_STA2    0x100010ACu
#define GATE_USB_BIT  1u

/* the three blocks the DTB names */
#define SSUSB_BASE 0x11200000u
#define SSUSB_IPPC 0x11203E00u
#define SSUSB_SIF2 0x11F40000u

/* IPPC (LK's header; layout confirmed by the kernel's struct) */
#define IPPC_PW_CTRL0  0x00            /* bit0 = IP_SW_RST                  */
#define IPPC_PW_CTRL1  0x04            /* bit0 = IP_HOST_PDN                */
#define IPPC_PW_CTRL2  0x08            /* bit0 = IP_DEV_PDN                 */
#define IPPC_PW_STS1   0x10
#define IPPC_XHCI_CAP  0x24            /* [7:0] u3 ports, [15:8] u2 ports   */
#define IPPC_U3_CTRL(i) (0x30 + 8u * (i))
#define IPPC_U2_CTRL(i) (0x50 + 8u * (i))
#define IPPC_HW_ID     0xA0

#define CTRL0_IP_SW_RST (1u << 0)
#define CTRL1_HOST_PDN  (1u << 0)
#define CTRL2_DEV_PDN   (1u << 0)
#define PORT_DIS        (1u << 0)
#define PORT_PDN        (1u << 1)
#define PORT_HOST_SEL   (1u << 2)

#define STS1_READY ((1u << 0) | (1u << 8) | (1u << 10) | (1u << 11))

/* the U2 T-PHY (phy-mt65xx-usb3.c; COM block at sif2 + 0x300) */
#define U2COM          (SSUSB_SIF2 + 0x300)
#define PHY_ACR5       (U2COM + 0x14)
#define PHY_ACR6       (U2COM + 0x18)
#define PHY_DTM0       (U2COM + 0x68)
#define PHY_DTM1       (U2COM + 0x6C)

#define PA5_HSTX_SRCTRL     (7u << 12)
#define PA5_HSTX_SRCTRL_4   (4u << 12)
#define PA6_OTG_VBUSCMP_EN  (1u << 20)
#define P2C_FORCE_SUSPENDM  (1u << 18)
#define P2C_RG_DATAIN       (0xFu << 10)
#define P2C_RG_XCVRSEL      (3u << 4)
#define P2C_DTM0_PART_MASK  ((1u << 23) | (1u << 21) | (1u << 20) | (1u << 19) \
                             | (1u << 17) | (1u << 7) | (1u << 6) | (1u << 2))
#define P2C_RG_VBUSVALID    (1u << 5)
#define P2C_RG_SESSEND      (1u << 4)
#define P2C_RG_AVALID       (1u << 2)

static void spin_us(c64_u32 us)
{
    c64_u64 hz = c64_cnt_freq();
    if (!hz)
        hz = 13000000ull;
    c64_u64 until = c64_cnt_now() + ((hz / 1000000ull) * us) + 1;
    while (c64_cnt_now() < until)
        ;
}

/* Is there an SSUSB block at all? Unassigned MMIO reads all-ones (QEMU's
 * virt board has nothing here), and a gated block would hang rather than
 * answer, so the gate is checked before the ID is read. */
int c64_ssusb_present(void)
{
    if ((R32(INFRA_STA2) >> GATE_USB_BIT) & 1) {
        R32(INFRA_CLR2) = 1u << GATE_USB_BIT;
        __asm__ volatile("dsb sy" ::: "memory");
        spin_us(100);
        if ((R32(INFRA_STA2) >> GATE_USB_BIT) & 1) {
            c64_log("ssusb: infra_usb will not ungate -- no USB host\n");
            return 0;
        }
    }
    return R32(SSUSB_IPPC + IPPC_HW_ID) != 0xFFFFFFFFu;
}

/* Bring the host IP up in host role. Returns 1 when the STS1 clock-stable
 * bits are met and the xHCI register file is answering, 0 otherwise. */
int c64_ssusb_host_up(void)
{
    c64_u32 v, xcap, s1;
    int u3, u2, i;

    c64_logf("ssusb: HW_ID=%08x PW_CTRL0=%08x CTRL1=%08x at handover\n",
             R32(SSUSB_IPPC + IPPC_HW_ID), R32(SSUSB_IPPC + IPPC_PW_CTRL0),
             R32(SSUSB_IPPC + IPPC_PW_CTRL1));

    /* the IP arrives held in reset: pulse it so it starts from a defined
     * state whichever way it was left */
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL0);
    R32(SSUSB_IPPC + IPPC_PW_CTRL0) = v | CTRL0_IP_SW_RST;
    __asm__ volatile("dsb sy" ::: "memory");
    spin_us(10);
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL0);
    R32(SSUSB_IPPC + IPPC_PW_CTRL0) = v & ~CTRL0_IP_SW_RST;
    __asm__ volatile("dsb sy" ::: "memory");
    spin_us(10);

    /* the device IP powers on by default, and leaving it on is what stops
     * the host side owning the port */
    v = R32(SSUSB_IPPC + IPPC_PW_CTRL2);
    R32(SSUSB_IPPC + IPPC_PW_CTRL2) = v | CTRL2_DEV_PDN;

    v = R32(SSUSB_IPPC + IPPC_PW_CTRL1);
    R32(SSUSB_IPPC + IPPC_PW_CTRL1) = v & ~CTRL1_HOST_PDN;

    /* port counts read AFTER the reset -- the ones read while the IP was
     * held are not evidence of anything */
    xcap = R32(SSUSB_IPPC + IPPC_XHCI_CAP);
    u3 = (int)(xcap & 0xFF);
    u2 = (int)((xcap >> 8) & 0xFF);
    if (u3 > 4) u3 = 4;
    if (u2 > 5) u2 = 5;
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

    for (i = 0; i < 200; i++) {
        s1 = R32(SSUSB_IPPC + IPPC_PW_STS1);
        if ((s1 & STS1_READY) == STS1_READY)
            break;
        spin_us(100);
    }
    s1 = R32(SSUSB_IPPC + IPPC_PW_STS1);
    if ((s1 & STS1_READY) != STS1_READY) {
        c64_logf("ssusb: clocks NOT stable after host enable (PW_STS1=%08x)\n",
                 s1);
        return 0;
    }

    /* the U2 T-PHY into host mode: stop forcing suspend and the
     * transceiver's pull-downs, OTG VBUS comparator on, then the host
     * declaration (VBUSVALID + AVALID set, SESSEND clear) and the vendor's
     * default slew rate */
    v = R32(PHY_DTM0);
    v &= ~(P2C_FORCE_SUSPENDM | P2C_RG_XCVRSEL);
    v &= ~(P2C_RG_DATAIN | P2C_DTM0_PART_MASK);
    R32(PHY_DTM0) = v;
    v = R32(PHY_ACR6);
    R32(PHY_ACR6) = v | PA6_OTG_VBUSCMP_EN;
    v = R32(PHY_DTM1);
    v |= P2C_RG_VBUSVALID | P2C_RG_AVALID;
    v &= ~P2C_RG_SESSEND;
    R32(PHY_DTM1) = v;
    v = R32(PHY_ACR5);
    R32(PHY_ACR5) = (v & ~PA5_HSTX_SRCTRL) | PA5_HSTX_SRCTRL_4;
    __asm__ volatile("dsb sy" ::: "memory");

    c64_logf("ssusb: host up -- %d u2 port(s), %d u3 port(s), PW_STS1=%08x, "
             "xHCI CAPLENGTH=%02x HCIVERSION=%04x\n", u2, u3, s1,
             R32(SSUSB_BASE) & 0xFF, (R32(SSUSB_BASE) >> 16) & 0xFFFF);
    return (R32(SSUSB_BASE) & 0xFF) == 0x20;
}
