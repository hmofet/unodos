/* ===========================================================================
 * unovdev_net - what is on the other end of the guest's network cable.
 *
 * The virtio-net TRANSPORT is in unovdev.c beside the console and the disk,
 * because it is ring machinery like they are.  This file is the opposite: no
 * virtio, no virtqueues, no hypervisor - one function that takes an Ethernet
 * frame and returns the frame that answers it, if any.  It is a peer on a
 * wire, and it is testable with two byte arrays and no machine at all.
 *
 * WHY A SYNTHETIC PEER RATHER THAN THE REAL WIRE, and this is a scope
 * decision worth stating rather than discovering:
 *
 *   - The test box runs QEMU with `-nic none`.  There is no network device
 *     for UnoDOS to have, let alone to share, so a bridge would be untestable
 *     exactly where the hypervisor is testable.
 *   - `uno_nic_t`'s `recv()` is DESTRUCTIVE - it hands over a packet and
 *     forgets it - so the guest and `unonet` cannot both poll one NIC.  A
 *     real bridge has to sit between the driver and `net_init()`, in files
 *     two other lanes own, and it has to answer the one-MAC-two-IP question
 *     (docs/UNOVIRT-PLAN.md R3) before it can carry anyone's traffic.
 *
 * So this slice proves the DEVICE - both directions of the ring, the header,
 * the receive-buffer discipline, the interrupt - against a peer that answers.
 * A guest that ARPs for its gateway and gets a reply, then pings it and gets
 * an echo, has exercised every part of the datapath except the wire.  The
 * bridge is the next slice, and it is a networking problem rather than a
 * virtualization one.  Track B's stub agent is the same trick (§4.5).
 * ======================================================================== */
#include "unovdev.h"

typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* The little network the guest finds itself on.  Private, ours, and unrelated
 * to whatever the host machine is really on - which is the point: nothing
 * here touches a real network, so nothing here can collide with one. */
#define NET_HOST_IP_0 10
#define NET_HOST_IP_1 77
#define NET_HOST_IP_2 0
#define NET_HOST_IP_3 1

static const u8 HOST_MAC[6]  = { 0x02, 0x55, 0x4E, 0x4F, 0x00, 0x01 };
static const u8 GUEST_MAC[6] = { 0x02, 0x55, 0x4E, 0x4F, 0x00, 0x02 };
static const u8 HOST_IP[4]   = { NET_HOST_IP_0, NET_HOST_IP_1,
                                 NET_HOST_IP_2, NET_HOST_IP_3 };

const unsigned char *uno_vnet_guest_mac(void) { return GUEST_MAC; }

static struct { int arp, icmp, other, replies; } N;

static u16 rd16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static void copy(u8 *d, const u8 *s, int n) { int i; for (i = 0; i < n; i++) d[i] = s[i]; }
static int  same(const u8 *a, const u8 *b, int n)
{ int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

/* The one's-complement sum every IP header and every ICMP message carries.
 * Computed over the buffer with the checksum field ALREADY ZEROED, which is
 * the step that is easy to leave out and produces a packet the guest drops in
 * silence - there is no error for a bad checksum, only a reply that never
 * arrives. */
static u16 csum(const u8 *p, int n)
{
    u32 s = 0;
    int i;
    for (i = 0; i + 1 < n; i += 2) s += (u32)rd16(p + i);
    if (i < n) s += (u32)p[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

#define ETH_HDR   14
#define ET_ARP    0x0806
#define ET_IPV4   0x0800

/* Answer an ARP request for our address.  A guest cannot send a single IP
 * packet until this works: it has the gateway's address and no idea what to
 * put in the Ethernet header until somebody claims it. */
static int arp_reply(const u8 *in, int len, u8 *out, int cap)
{
    const u8 *a = in + ETH_HDR;
    if (len < ETH_HDR + 28 || cap < ETH_HDR + 28) return 0;
    if (rd16(a) != 1 || rd16(a + 2) != ET_IPV4) return 0;   /* ethernet/ip   */
    if (a[4] != 6 || a[5] != 4) return 0;
    if (rd16(a + 6) != 1) return 0;                          /* a request?    */
    if (!same(a + 24, HOST_IP, 4)) return 0;                 /* for US?       */

    copy(out, in + 6, 6);                    /* to whoever asked             */
    copy(out + 6, HOST_MAC, 6);
    wr16(out + 12, ET_ARP);
    {   u8 *r = out + ETH_HDR;
        wr16(r, 1); wr16(r + 2, ET_IPV4);
        r[4] = 6; r[5] = 4;
        wr16(r + 6, 2);                      /* a reply                      */
        copy(r + 8, HOST_MAC, 6);
        copy(r + 14, HOST_IP, 4);
        copy(r + 18, a + 8, 6);              /* back to the asker            */
        copy(r + 24, a + 14, 4);
    }
    N.arp++;
    return ETH_HDR + 28;
}

/* Answer an ICMP echo, which is the whole point of the exercise: a reply can
 * only arrive if the frame left the guest through the transmit queue, reached
 * us, and our answer went back through a receive buffer the guest had posted
 * and an interrupt it took. */
static int icmp_reply(const u8 *in, int len, u8 *out, int cap)
{
    const u8 *ip = in + ETH_HDR;
    int ihl, iplen, icmplen;
    u8 *oip, *oic;

    if (len < ETH_HDR + 20) return 0;
    if ((ip[0] >> 4) != 4) return 0;
    ihl = (ip[0] & 15) * 4;
    if (ihl < 20 || len < ETH_HDR + ihl + 8) return 0;
    if (ip[9] != 1) return 0;                                /* ICMP?         */
    if (!same(ip + 16, HOST_IP, 4)) return 0;                /* to US?        */
    iplen = (int)rd16(ip + 2);
    if (iplen < ihl || ETH_HDR + iplen > len) return 0;
    icmplen = iplen - ihl;
    if (in[ETH_HDR + ihl] != 8) return 0;                    /* echo request  */
    if (cap < ETH_HDR + 20 + icmplen) return 0;

    copy(out, in + 6, 6);
    copy(out + 6, HOST_MAC, 6);
    wr16(out + 12, ET_IPV4);

    oip = out + ETH_HDR;
    oip[0] = 0x45; oip[1] = 0;
    wr16(oip + 2, (u16)(20 + icmplen));
    wr16(oip + 4, 0); wr16(oip + 6, 0);
    oip[8] = 64; oip[9] = 1;
    wr16(oip + 10, 0);                       /* zero BEFORE summing          */
    copy(oip + 12, HOST_IP, 4);
    copy(oip + 16, ip + 12, 4);              /* back where it came from      */
    wr16(oip + 10, csum(oip, 20));

    oic = oip + 20;
    {   int i;
        for (i = 0; i < icmplen; i++) oic[i] = in[ETH_HDR + ihl + i];
    }
    oic[0] = 0;                              /* echo REPLY                   */
    oic[1] = 0;
    wr16(oic + 2, 0);
    wr16(oic + 2, csum(oic, icmplen));
    N.icmp++;
    return ETH_HDR + 20 + icmplen;
}

/* One frame in, at most one frame out.  Everything it does not understand is
 * DROPPED rather than answered, and counted: a guest talking to a peer that
 * invents replies is worse off than one talking to a peer that says nothing,
 * because it will believe them. */
int uno_vnet_respond(const unsigned char *in, int len,
                     unsigned char *out, int cap)
{
    u16 type;
    int n = 0;
    if (len < ETH_HDR) return 0;
    /* Ours or a broadcast; anything else is not for this peer. */
    if (!same(in, HOST_MAC, 6) && in[0] != 0xFF) return 0;
    type = rd16(in + 12);
    if (type == ET_ARP)       n = arp_reply(in, len, out, cap);
    else if (type == ET_IPV4) n = icmp_reply(in, len, out, cap);
    else N.other++;
    if (n) N.replies++;
    return n;
}

int uno_vnet_str(char *buf, int cap)
{
    int i = 0;
    static const char H[] = "0123456789abcdef";
    /* Hand-rolled, because this is the one line that says whether the guest's
     * traffic ever reached us and printf-family formatting is not worth a
     * dependency here. */
    const int vals[4] = { N.arp, N.icmp, N.other, N.replies };
    const char *names[4] = { "arp ", " icmp ", " other ", " replied " };
    int k;
    for (k = 0; k < 4 && i + 12 < cap; k++) {
        const char *s = names[k];
        int v = vals[k], d = 1000000, seen = 0;
        while (*s && i + 1 < cap) buf[i++] = *s++;
        if (!v && i + 1 < cap) buf[i++] = '0';
        while (d && i + 1 < cap) {
            int q = (v / d) % 10;
            if (q || seen) { buf[i++] = H[q]; seen = 1; }
            d /= 10;
        }
    }
    if (i < cap) buf[i] = 0;
    return i;
}
