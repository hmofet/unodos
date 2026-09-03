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
 * calls renamed onto usb.c, which bounces them through uncached memory and
 * turns the receive path asynchronous.
 *
 * WiFi is not on the list and will not be: the radio is MediaTek CONNSYS,
 * entangled with consys/CCCI platform devices, and there is no route to it
 * from bare metal (research/mainline-port-plan.md in hmofet/cosmo).
 *
 * TWO RULES THIS FILE LEARNED FROM ITS FIRST HARDWARE BOOT, both about the
 * fact that it runs ON THE SHELL'S FRAME LOOP:
 *
 *  1. THE BUDGET IS WALL CLOCK, NOT ITERATIONS. The first cut copied x86's
 *     `for (i = 0; i < 600; i++) { ...; budget -= 5; }`, which assumes each
 *     pass costs about the 5 ms it sleeps. Here a USB transfer that gets no
 *     answer costs poll_xfer's full FIVE SECOND timeout, so an "8 second"
 *     budget was really hours. Every loop below is bounded by CNTPCT instead,
 *     so the worst case is the deadline plus one in-flight call.
 *  2. BIND THE STACK ONLY ONCE THE LINK IS UP. net_init() was called before
 *     the link wait, so a bring-up that failed still left the adapter bound --
 *     and the shell then called net_poll() four times a frame forever after,
 *     each one draining a receive path that had nothing to give. Failure now
 *     leaves the stack unbound and net_poll() returns at its first line.
 */

#include "cosmo64.h"
#include "net.h"
#include "xhci.h"          /* uno_usb_dev: the ASIX's index and USB speed */
#include "ax88179.h"

void uno_pc64_delay_ms(int ms);

static int g_inited;

/* ax88179.c narrates its bring-up through uno_dbg_net_trace(), which
 * uno_debug.h compiles to ((void)0) outside a debug build. Those lines are
 * exactly the ones a hardware boot needs, and stubbing them silent is why the
 * first M5 log could not say how far the adapter got. */
void uno_dbg_net_trace(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    c64_logv(fmt, ap);
    __builtin_va_end(ap);
    c64_log("\n");
}

/* ---- looking at the adapter directly ------------------------------------- *
 * ax88179.c exposes found/bound/link and nothing else, and "tx=4 rx=0" from
 * the net stack cannot say whether the MAC's receiver is even enabled. These
 * read the chip's own registers over the same AX_ACCESS_MAC vendor control
 * transfer the driver uses, through usb.c's bounce, without touching the
 * driver. */
int c64_usb_control(int dev, unsigned char rt, unsigned char req,
                    unsigned short val, unsigned short idx, void *data, int len);
int c64_usb_bulk_probe(int dev, int len);   /* usb.c: a synchronous bulk IN */
int uno_xhci_dev_count(void);
const uno_usb_dev *uno_xhci_dev(int i);

#define AX_ACCESS_MAC          0x01
#define AX_RX_CTL              0x0B
#define AX_MEDIUM_STATUS_MODE  0x22
#define AX_RX_BULKIN_QCTRL     0x2E

static int g_ax = -1;                        /* xHCI device index, or -1 */
static int g_ax_speed;

static int asix_find(void)
{
    int i, n;
    if (g_ax >= 0)
        return g_ax;
    n = uno_xhci_dev_count();
    for (i = 0; i < n; i++) {
        const uno_usb_dev *d = uno_xhci_dev(i);
        if (d && d->vendor == 0x0b95) {
            g_ax = i;
            g_ax_speed = d->speed;
            return g_ax;
        }
    }
    return -1;
}

static int ax_rd(int reg, int size, void *buf)
{
    int d = asix_find();
    if (d < 0)
        return -1;
    return c64_usb_control(d, 0xC0, AX_ACCESS_MAC, (unsigned short)reg,
                           (unsigned short)size, buf, size);
}

static int ax_wr(int reg, int size, void *buf)
{
    int d = asix_find();
    if (d < 0)
        return -1;
    return c64_usb_control(d, 0x40, AX_ACCESS_MAC, (unsigned short)reg,
                           (unsigned short)size, buf, size);
}

static void asix_dump(const char *when)
{
    unsigned char rxctl[2] = {0, 0}, med[2] = {0, 0}, q[5] = {0, 0, 0, 0, 0};
    ax_rd(AX_RX_CTL, 2, rxctl);
    ax_rd(AX_MEDIUM_STATUS_MODE, 2, med);
    ax_rd(AX_RX_BULKIN_QCTRL, 5, q);
    c64_logf("asix[%s]: usb-speed=%d RX_CTL=%02x%02x MEDIUM=%02x%02x "
             "BULKIN_QCTRL=%02x %02x %02x %02x %02x\n", when, g_ax_speed,
             rxctl[1], rxctl[0], med[1], med[0], q[0], q[1], q[2], q[3], q[4]);
    c64_log_flush();
}

/* THE AGGREGATION SIZE MUST MATCH THE BUFFER THE DRIVER SUBMITS.
 *
 * Boot 3 measured it. Transmit is provably on the wire (a LAN capture caught
 * every DHCP DISCOVER), the DHCP server provably serves this MAC (Linux on
 * this same adapter holds a lease from it), and the receive side reported
 * `arm=1 poll=6400 land=0 err=0`: one TRB posted, never completed, never
 * failed. Not a scheduling problem either -- the bulk OUT endpoint on the same
 * device completed 22 of 22 transfers, so this controller does service bulk.
 * The chip is simply never sending.
 *
 * AX_RX_BULKIN_QCTRL's fourth byte is why. It reads back at exactly HALF what
 * is written (0x12 -> 0x09, 0x18 -> 0x0c, measured both times), so the chip
 * holds that field in 2 KB units -- and in the Linux driver it sets the
 * receive URB size as `1024 * (size + 2)`. ax88179.c writes 0x12, which asks
 * the device to aggregate into a TWENTY KILOBYTE burst. ax_recv() submits
 * `sizeof g_rx` = FOUR kilobytes. The device will not hand over a burst the
 * host has no room for, so it hands over nothing at all, forever, without an
 * error -- which is exactly the signature above.
 *
 * Every x86 box this driver has run on submits the same 4 KB and would have
 * the same mismatch, so this is not Cosmo-specific and not about USB speed
 * (boot 3's guess). It has simply never been noticed, presumably because
 * nothing on the fleet has needed the ASIX to receive.
 *
 * So: ask for size 0x02 -> 1024 * (2 + 2) = 4 KB, matching g_rx exactly. The
 * timer and IFG bytes come from Linux's non-SuperSpeed entry, which is what a
 * high-speed host should be using anyway (the Cosmo's SSUSB reports one U2
 * port and no U3 port, and the adapter enumerates at high speed).
 *
 * Applied from HERE rather than in the driver on purpose: it is a measurement
 * this lane can make and the x86 lane cannot, and changing ax88179.c's own
 * table is the pc64 lane's call. If this is what fixes RX it goes to them as a
 * request with the numbers attached. ax_apply_medium() only rewrites on a mode
 * CHANGE, so once the link is stable this write stands. */
static void asix_match_aggregation(void)
{
    /* ctrl, timer_lo, timer_hi, size, ifg -- size 0x02 => a 4 KB burst */
    unsigned char q[5] = { 0x07, 0xCC, 0x4C, 0x02, 0x08 };
    if (ax_wr(AX_RX_BULKIN_QCTRL, 5, q) < 0) {
        c64_log("asix: could not reprogram the bulk-in queue\n");
        return;
    }
    c64_log("asix: bulk-in aggregation set to 4 KB (size=02) to match the "
            "driver's 4 KB receive buffer\n");
    c64_log_flush();
}

/* ---- a real deadline ----------------------------------------------------- */
static c64_u64 g_deadline;

static void deadline_in(int ms)
{
    c64_u64 hz = c64_cnt_freq();
    if (!hz)
        hz = 13000000ull;
    g_deadline = c64_cnt_now() + (hz / 1000ull) * (c64_u64)ms;
}

static int expired(void)
{
    return c64_cnt_now() >= g_deadline;
}

/* Every stage is logged AND FLUSHED before the call that could block, because
 * the log's push to the eMMC rides the poll loop -- so anything that wedges
 * the frame loop also stops the log reaching storage, and the breadcrumb has
 * to be written before the step rather than after it. */
static void stage(const char *s)
{
    c64_logf("net: %s\n", s);
    c64_log_flush();
}

/* Three failure modes need telling apart, and a hardware boot gets one log. */
static void report(const char *what)
{
    int found = 0, bound = 0, link = 0;
    unsigned short vid = 0, pid = 0;
    ax88179_status(&found, &bound, &link, &vid, &pid);
    c64_logf("net: %s -- ax88179 found=%d bound=%d link=%d %04x:%04x, "
             "tx=%d rx=%d\n", what, found, bound, link, vid, pid,
             (int)net_tx_frames(), (int)net_rx_frames());
    c64_log_flush();
}

int pc64_net_boot(void)
{
    uno_nic_t *nic;

    if (g_inited || net_dhcp_done()) {
        g_inited = 1;
        return 1;
    }

    deadline_in(8000);
    stage("probing for a USB Ethernet adapter");
    nic = ax88179_nic();
    if (!nic) {
        report("no adapter");
        return 0;
    }

    /* Link BEFORE net_init, for two reasons. ax88179.c programs the MAC
     * medium from the PHY's negotiated speed inside nic->link(), and a medium
     * that does not match the link silently kills RX -- the tx>0 rx=0
     * signature the x86 lane hit on a 100M port. And binding an adapter that
     * never comes up leaves the shell draining it every frame forever. */
    if (nic->link) {
        stage("waiting for link");
        while (!expired() && !nic->link(nic->ctx))
            uno_pc64_delay_ms(5);
        if (!nic->link(nic->ctx)) {
            report("no link (stack left UNBOUND)");
            return 0;
        }
    }
    report("link up");
    asix_dump("after link");

    /* ---- THREE MEASUREMENTS, then stop guessing -------------------------
     * Boot 4 killed the aggregation-size theory: the write landed (the size
     * field read back as 01, half of the 02 written, exactly the 2 KB-unit
     * behaviour) and the receive path still reported arm=1 land=0 err=0.
     * Boot 3 had already killed the USB-speed theory. Both were inferences
     * from a register dump, and neither asked the controller anything.
     *
     * These do. Each is a SYNCHRONOUS bulk IN, whose failure path in xhci.c
     * prints the completion code and the endpoint's state word -- the two
     * facts that separate "the endpoint is not running" from "the endpoint is
     * running and the device is silent", which is the fork every theory so far
     * has had to guess at. They cost a 5-second timeout each when they fail,
     * so they run once, here, before the stack is bound.
     *
     *   A: 4 KB with the chip's DEFAULT aggregation (size 0x12 => the device
     *      wants to burst 20 KB). This is what ax_recv() does today.
     *   B: 4 KB with aggregation matched to 4 KB (size 0x02). If A fails and B
     *      works, the size theory was right and something in the async path is
     *      the remaining problem.
     *   C: 20 KB with the DEFAULT aggregation restored -- the burst the chip
     *      actually asked for. If C works where A fails, the driver's 4 KB
     *      receive buffer is simply too small and the fix is its size, not the
     *      chip's.
     *
     * If all three fail with the same completion code, the ASIX is not the
     * problem at all and the bulk-IN endpoint is not being serviced -- which
     * would make this the first bulk IN ever attempted on this controller
     * (control IN, interrupt IN and bulk OUT are all proven; bulk IN is the
     * one direction M4 never exercised). */
    {
        unsigned char dflt[5] = { 0x07, 0x4F, 0x00, 0x12, 0xFF };  /* 20 KB */
        int d = asix_find();
        c64_usb_bulk_probe(d, 4096);                       /* A */

        asix_match_aggregation();
        asix_dump("after 4 KB aggregation");
        c64_usb_bulk_probe(d, 4096);                       /* B */

        ax_wr(AX_RX_BULKIN_QCTRL, 5, dflt);
        asix_dump("after default aggregation");
        c64_usb_bulk_probe(d, 20480);                      /* C */

        /* Leave the chip matched to what ax_recv() will actually submit. */
        asix_match_aggregation();
    }

    /* A fresh window for DHCP: the link wait has already spent part of the
     * first one, and how long autoneg took says nothing about how long the
     * server will take. */
    deadline_in(6000);
    stage("DHCP");
    net_init(nic, ax88179_mac());
    /* net_dhcp_start sends one DISCOVER; net_poll's dhcp_tick retransmits it
     * (and the REQUEST) about every 1.5 s, so a lost OFFER or ACK recovers
     * inside this window. */
    net_dhcp_start();
    while (!expired() && !net_dhcp_done()) {
        net_poll();
        uno_pc64_delay_ms(5);
    }

    if (!net_dhcp_done()) {
        /* Leave the adapter bound: the link IS up, net_poll's receive path is
         * asynchronous and therefore cheap, and dhcp_tick keeps retransmitting
         * from the shell's own net_poll calls, so a late server still leases. */
        report("no lease yet (link up, stack bound, DHCP still retrying)");
        return 0;
    }

    {
        const unsigned char *ip = net_ip(), *gw = net_gw();
        c64_logf("net: LEASED %d.%d.%d.%d gw %d.%d.%d.%d, link %d Mbps, "
                 "tx=%d rx=%d\n",
                 ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3],
                 net_link_speed_mbps(), (int)net_tx_frames(),
                 (int)net_rx_frames());
        c64_log_flush();
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
