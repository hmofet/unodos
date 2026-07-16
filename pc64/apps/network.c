/* Network app module (APP_NETWORK) - pc64's networking self-test + status.
 *
 * Drives the native e1000 driver + the pc64 TCP/IP stack (both linked into
 * the image) through a scripted sequence, showing each capability's result:
 * link, DHCP lease, ICMP ping, a UDP echo, and a TCP echo. The echo peers are
 * QEMU SLIRP guestfwd targets (10.0.2.100 :9001 udp / :9000 tcp -> host cat),
 * so the whole path is exercised end-to-end and screenshot-verifiable.
 *
 * pc64-only (like Settings): it calls the net_ and e1000_ functions
 * directly - they are kernel-side symbols in the same statically-linked
 * UEFI image.
 */
#include "uno_mod.h"
#include "net.h"
#include "e1000.h"

/* SLIRP peers: the gateway runs a built-in TFTP server (UDP round-trip proof,
   fully self-contained); 10.0.2.100 is a guestfwd -> host `cat` (TCP echo). */
static const u8 GW[4]   = {10, 0, 2, 2};
static const u8 ECHO[4] = {10, 0, 2, 100};
#define TFTP_PORT 69
#define TCP_PORT  9000
#define SPORT     5000

/* build a TFTP Read Request for "uno.txt" in octet mode */
static int tftp_rrq(u8 *o)
{
    const char *fn = "uno.txt", *md = "octet";
    int i = 0, j;
    o[i++] = 0; o[i++] = 1;                      /* opcode RRQ */
    for (j = 0; fn[j]; j++) o[i++] = fn[j]; o[i++] = 0;
    for (j = 0; md[j]; j++) o[i++] = md[j]; o[i++] = 0;
    return i;
}

enum { S_INIT, S_DHCP, S_PING, S_UDP, S_TCP, S_DONE, S_NONIC };
enum { R_WAIT = 0, R_OK = 1, R_FAIL = 2 };

static int  gStep, gTimer, gStarted;
static int  gRes[5];                 /* dhcp, ping, udp, tcp results (+link) */
static char gLease[20], gMacStr[20];
static char gUdpEcho[24], gTcpEcho[24];

static void hex2(char *o, unsigned v) {
    const char *h = "0123456789ABCDEF";
    o[0] = h[(v >> 4) & 15]; o[1] = h[v & 15];
}
static void fmt_ip(char *o, const u8 *ip) {
    char n[12]; int i, j = 0;
    for (i = 0; i < 4; i++) {
        fmt_u(ip[i], n);
        { int k = 0; while (n[k]) o[j++] = n[k++]; }
        if (i < 3) o[j++] = '.';
    }
    o[j] = 0;
}

static void net_reset(void)
{
    uno_nic_t *nic;
    gStep = S_INIT; gTimer = 0;
    gRes[0]=gRes[1]=gRes[2]=gRes[3]=gRes[4]=R_WAIT;
    gLease[0]=gMacStr[0]=gUdpEcho[0]=gTcpEcho[0]=0;
    nic = e1000_nic();
    if (!nic) { gStep = S_NONIC; return; }
    {
        const u8 *m = e1000_mac(); int i, j = 0;
        for (i = 0; i < 6; i++) { hex2(gMacStr + j, m[i]); j += 2;
            if (i < 5) gMacStr[j++] = ':'; }
        gMacStr[j] = 0;
    }
    net_init(nic, e1000_mac());
    gRes[4] = net_link() ? R_OK : R_WAIT;
    net_dhcp_start();
    gStep = S_DHCP; gTimer = 0;
}

/* one step of the sequence; called from tick after pumping net_poll */
static void net_step(void)
{
    gTimer++;
    if (gRes[4] != R_OK && net_link()) gRes[4] = R_OK;
    switch (gStep) {
    case S_DHCP:
        if (net_dhcp_done()) { gRes[0] = R_OK; fmt_ip(gLease, net_ip());
            gStep = S_PING; gTimer = 0; net_ping(GW); }
        else if (gTimer > 150) { gRes[0] = R_FAIL; fmt_ip(gLease, net_ip());
            gStep = S_PING; gTimer = 0; net_ping(GW); }
        break;
    case S_PING: {
        u8 rrq[32]; int rn = tftp_rrq(rrq);
        if (net_ping_replied()) { gRes[1] = R_OK; gStep = S_UDP; gTimer = 0;
            net_udp_send(GW, TFTP_PORT, SPORT, rrq, rn); }
        else if (gTimer > 120) { gRes[1] = R_FAIL; gStep = S_UDP; gTimer = 0;
            net_udp_send(GW, TFTP_PORT, SPORT, rrq, rn); }
        else if ((gTimer % 40) == 0) net_ping(GW);
        break;
    }
    case S_UDP: {
        u8 src[4]; u16 sp; char buf[32];
        int n = net_udp_recv(SPORT, buf, sizeof buf - 1, src, &sp);
        if (n >= 4 && buf[1] == 3) {            /* TFTP DATA (opcode 3) */
            int i, j = 0;
            for (i = 4; i < n && j < 23; i++) gUdpEcho[j++] = buf[i];
            gUdpEcho[j] = 0;
            gRes[2] = R_OK; gStep = S_TCP; gTimer = 0; net_tcp_connect(ECHO, TCP_PORT); }
        else if (gTimer > 120) { gRes[2] = R_FAIL; gStep = S_TCP; gTimer = 0;
            net_tcp_connect(ECHO, TCP_PORT); }
        else if ((gTimer % 40) == 0) {
            u8 rrq[32]; int rn = tftp_rrq(rrq);
            net_udp_send(GW, TFTP_PORT, SPORT, rrq, rn); }
        break;
    }
    case S_TCP: {
        static int sent;
        if (gTimer == 1) sent = 0;
        if (!sent && net_tcp_state() == TCP_ESTABLISHED) {
            net_tcp_send("UNODOS-TCP", 10); sent = 1;
        }
        if (sent) {
            char buf[24];
            int n = net_tcp_recv(buf, sizeof buf - 1);
            if (n > 0) { buf[n] = 0; { int i; for(i=0;i<=n&&i<23;i++) gTcpEcho[i]=buf[i]; }
                gRes[3] = R_OK; net_tcp_close(); gStep = S_DONE; }
        }
        if (gStep == S_TCP && gTimer > 150) { gRes[3] = R_FAIL; net_tcp_close();
            gStep = S_DONE; }
        break;
    }
    default: break;
    }
}

static void network_tick(void)
{
    int i;
    if (gStep == S_NONIC || gStep == S_DONE) { net_poll(); return; }
    for (i = 0; i < 4; i++) net_poll();      /* pump the stack */
    net_step();
}

static const char *stat_str(int r)
{ return r == R_OK ? "ok" : r == R_FAIL ? "FAIL" : ".."; }
static short stat_col(int r)
{ return r == R_OK ? C_CYAN : r == R_FAIL ? C_MAG : C_WHITE; }

static void row(short x, short y, const char *label, int res, const char *extra)
{
    text_at(x, y, label, C_WHITE, C_BLUE, false);
    text_at(x + 130, y, stat_str(res), stat_col(res), C_BLUE, false);
    if (extra && extra[0]) text_at(x + 180, y, extra, C_CYAN, C_BLUE, false);
}

static void network_draw(UnoWin *w)
{
    Rect r = w->bounds;
    short x = r.left + 10, y = r.top + TBAR_H + 14;
    text_at(x, y, "e1000 Networking", C_MAG, C_BLUE, false); y += 18;
    if (gStep == S_NONIC) {
        text_at(x, y, "No e1000 NIC found (need -device e1000).", C_WHITE, C_BLUE, false);
        return;
    }
    text_at(x, y, "MAC", C_WHITE, C_BLUE, false);
    text_at(x + 40, y, gMacStr, C_CYAN, C_BLUE, false); y += 14;
    row(x, y, "Link", gRes[4], 0); y += 14;
    row(x, y, "DHCP lease", gRes[0], gLease); y += 14;
    row(x, y, "Ping 10.0.2.2", gRes[1], 0); y += 14;
    row(x, y, "UDP (TFTP)", gRes[2], gUdpEcho); y += 14;
    row(x, y, "TCP echo", gRes[3], gTcpEcho); y += 16;
    text_at(x, y, gStep == S_DONE ? "Done.  R: re-run" : "testing...",
            C_CYAN, C_BLUE, false);
}

static Boolean network_key(char ch, short code, Boolean cmd)
{
    (void)code; (void)cmd;
    if (ch == 'r' || ch == 'R') { net_reset(); gStarted = 1;
        { UnoWin *w = find_app_window(APP_NETWORK); if (w) draw_window(w); }
        return true; }
    return false;
}

static void network_opened(void)
{
    if (!gStarted) { net_reset(); gStarted = 1; }
}

static const AppInterface kIface = {
    network_draw, network_key, 0, network_tick, network_opened, 0,
    "Network", { 120, 46, 470, 250 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
