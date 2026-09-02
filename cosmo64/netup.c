/* cosmo64/netup.c -- network bring-up for the Cosmo (M5).
 *
 * This is the fourth thing that lived in a file cosmo64 replaces. The cursor
 * and the dirty-row present came out of uefi_main.c; this one comes out of
 * pc64_http.c, whose pc64_net_up() / pc64_net_boot() walk a table of eight
 * NIC families, half of them PCIe parts that cannot exist on this SoC, and
 * drag in TLS, the cookie jar, the HTTP cache and unolog behind them. The
 * shell calls pc64_net_boot() once at the top of its main loop and nothing
 * else in the linked set references the rest, so the seam is exactly one
 * function and this file provides it.
 *
 * There is one NIC family this machine can have: a USB Ethernet adapter on
 * the SSUSB host. The AX88179B is the one the hub M4 enumerated carries
 * (0b95:1790), and ax88179.c is compiled unchanged -- with its uno_usb_bulk_*
 * calls renamed onto usb.c's bounce, because those hand the caller's pointer
 * straight to the controller and the driver's buffers are cached .bss.
 *
 * WiFi is not on the list and will not be: the radio is MediaTek CONNSYS,
 * entangled with consys/CCCI platform devices, and there is no route to it
 * from bare metal (research/mainline-port-plan.md in hmofet/cosmo).
 */

#include "cosmo64.h"
#include "net.h"
#include "ax88179.h"

void uno_pc64_delay_ms(int ms);

static int g_inited;

/* Whether the adapter is even there, reported once and in full, because the
 * answer to "does networking work" has three quite different failure modes --
 * no adapter, no link, no lease -- and a hardware boot gets one log to tell
 * them apart. */
static void report(const char *stage, uno_nic_t *nic)
{
    int found = 0, bound = 0, link = 0;
    unsigned short vid = 0, pid = 0;
    ax88179_status(&found, &bound, &link, &vid, &pid);
    c64_logf("net: %s -- ax88179 found=%d bound=%d link=%d %04x:%04x, "
             "nic=%s, tx=%d rx=%d\n", stage, found, bound, link, vid, pid,
             nic ? "bound" : "none", (int)net_tx_frames(),
             (int)net_rx_frames());
}

int pc64_net_boot(void)
{
    uno_nic_t *nic;
    int i, budget = 8000;                 /* ms, the same bound x86 uses */

    if (g_inited || net_dhcp_done()) {
        g_inited = 1;
        return 1;
    }

    nic = ax88179_nic();
    if (!nic) {
        report("no adapter", 0);
        return 0;                         /* retried on the next call */
    }
    net_init(nic, ax88179_mac());

    /* Link BEFORE DHCP, and this is not merely tidy: ax88179.c programs the
     * MAC medium from the PHY's negotiated speed inside nic->link(), and a
     * medium that does not match the link silently kills RX -- the tx>0 rx=0
     * signature the x86 lane hit on a 100M port. ~3 s covers gigabit autoneg. */
    if (nic->link) {
        for (i = 0; i < 600 && budget > 0 && !nic->link(nic->ctx); i++) {
            uno_pc64_delay_ms(5);
            budget -= 5;
        }
        if (!nic->link(nic->ctx)) {
            report("no link", nic);
            return 0;
        }
    }
    report("link up", nic);

    /* net_dhcp_start sends one DISCOVER; net_poll's dhcp_tick retransmits it
     * (and the REQUEST) about every 1.5 s, so a lost OFFER or ACK recovers
     * inside this window. */
    net_dhcp_start();
    for (i = 0; i < 600 && budget > 0 && !net_dhcp_done(); i++) {
        net_poll();
        uno_pc64_delay_ms(5);
        budget -= 5;
    }

    if (!net_dhcp_done()) {
        report("no lease", nic);
        return 0;
    }

    {
        const unsigned char *ip = net_ip(), *gw = net_gw();
        c64_logf("net: LEASED %d.%d.%d.%d gw %d.%d.%d.%d, link %d Mbps, "
                 "tx=%d rx=%d\n",
                 ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3],
                 net_link_speed_mbps(), (int)net_tx_frames(),
                 (int)net_rx_frames());
    }
    g_inited = 1;
    return 1;
}

/* The lazy variant the rest of pc64 calls on first network use. Here it is the
 * same bring-up: there is only one device to try, so eager and lazy differ
 * only in when they run. */
int pc64_net_up(void)
{
    return pc64_net_boot();
}
