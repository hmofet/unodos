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
#include "tls.h"
#include "iwlwifi.h"      /* Intel WiFi status readout */
#include "rtwifi.h"       /* Realtek PCIe WiFi */
#include "mrvlwifi.h"     /* Marvell/NXP PCIe WiFi */
#include "e1000e.h"       /* Intel e1000e wired GbE (82574 / I219) */
#include "igb.h"          /* Intel igb wired GbE (I210/I211/82576) */
#include "r8169.h"        /* Realtek RTL8168/8111 wired GbE */

void uno_pc64_delay_ms(int ms);   /* kernel export: the firmware Stall */

/* The gateway (ping + TFTP-over-UDP target) comes from net_gw() so the test
   follows the DHCP-provided gateway on real hardware instead of a SLIRP-only
   literal; under QEMU SLIRP that gateway is 10.0.2.2 with a built-in TFTP
   server, so the QEMU path is unchanged. ECHO is the TCP/TLS echo peer - a
   QEMU guestfwd -> host `cat` with no real-hardware equivalent, so those two
   steps only complete under QEMU. On metal they black-hole, but each step is
   time-bounded (gTimer for TCP; the tls.c deadlines for TLS) so they fail fast
   rather than freeze the UI. */
static const u8 ECHO[4] = {10, 0, 2, 100};
#define TFTP_PORT 69
#define TCP_PORT  9000
#define TLS_PORT  9443
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

enum { S_INIT, S_DHCP, S_PING, S_UDP, S_TCP, S_TLS, S_DONE, S_NONIC };
enum { R_WAIT = 0, R_OK = 1, R_FAIL = 2 };

static int  gStep, gTimer, gStarted;
static int  gRes[6];                 /* dhcp,ping,udp,tcp,link,tls */
static char gLease[20], gMacStr[20];
static char gUdpEcho[24], gTcpEcho[24], gTlsInfo[40];
static char gWifiMsg[80];

/* THE APP IS A DIAGNOSTIC, AND IT USED TO OPEN LIKE ONE.
 *
 * It ran a five-step test suite on open - DHCP, ping, a TFTP fetch, a TCP echo
 * and a TLS handshake against QEMU-only peers - and printed the results, the
 * MAC, the negotiated cipher suite and the driver's own status string, above a
 * SECOND copy of the Wi-Fi join UI that the Control Panel already owns. Two
 * places to join a network, and the first screen of the OS's networking was a
 * test report with "TLS1.2 cs=49199" in it.
 *
 * So: the summary a person wants first, the tests behind a button for the
 * times someone is actually debugging the stack, and exactly one Wi-Fi UI in
 * this OS - the Control Panel's. */
static UiBtn gNetBtn;                       /* "Run tests"                     */

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
    uno_nic_t *nic; const u8 *m = 0;
    gStep = S_INIT; gTimer = 0;
    gRes[0]=gRes[1]=gRes[2]=gRes[3]=gRes[4]=gRes[5]=R_WAIT;
    gLease[0]=gMacStr[0]=gUdpEcho[0]=gTcpEcho[0]=gTlsInfo[0]=0;
    /* try each wired NIC in turn: e1000, e1000e (82574/I219), igb, Realtek 8168 */
    if      ((nic = e1000_nic()))  m = e1000_mac();
    else if ((nic = e1000e_nic())) m = e1000e_mac();
    else if ((nic = igb_nic()))    m = igb_mac();
    else if ((nic = r8169_nic()))  m = r8169_mac();
    if (!nic || !m) { gStep = S_NONIC; return; }
    {
        int i, j = 0;
        for (i = 0; i < 6; i++) { hex2(gMacStr + j, m[i]); j += 2;
            if (i < 5) gMacStr[j++] = ':'; }
        gMacStr[j] = 0;
    }
    net_init(nic, m);
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
            gStep = S_PING; gTimer = 0; net_ping(net_gw()); }
        else if (gTimer > 150) { gRes[0] = R_FAIL; fmt_ip(gLease, net_ip());
            gStep = S_PING; gTimer = 0; net_ping(net_gw()); }
        break;
    case S_PING: {
        u8 rrq[32]; int rn = tftp_rrq(rrq);
        if (net_ping_replied()) { gRes[1] = R_OK; gStep = S_UDP; gTimer = 0;
            net_udp_send(net_gw(), TFTP_PORT, SPORT, rrq, rn); }
        else if (gTimer > 120) { gRes[1] = R_FAIL; gStep = S_UDP; gTimer = 0;
            net_udp_send(net_gw(), TFTP_PORT, SPORT, rrq, rn); }
        else if ((gTimer % 40) == 0) net_ping(net_gw());
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
            net_udp_send(net_gw(), TFTP_PORT, SPORT, rrq, rn); }
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
                gRes[3] = R_OK; net_tcp_close(); gStep = S_TLS; gTimer = 0; }
        }
        if (gStep == S_TCP && gTimer > 150) { gRes[3] = R_FAIL; net_tcp_close();
            gStep = S_TLS; gTimer = 0; }
        break;
    }
    case S_TLS: {
        /* the whole TLS exchange runs synchronously (it pumps net_poll
           internally); one net_step tick is enough */
        char buf[32], num[12];
        int rc = tls_connect(ECHO, TLS_PORT, "unodos-pc64");
        if (rc == 0) {
            tls_write("UNODOS-TLS", 10);
            int n = tls_read(buf, sizeof buf - 1);
            if (n > 0 && tls_last_error() == 0) {
                unsigned v = tls_version(), c = tls_cipher();
                strcpy(gTlsInfo, (v == 0x0303) ? "TLS1.2 cs=" :
                                 (v == 0x0304) ? "TLS1.3 cs=" : "TLS cs=");
                fmt_u(c, num); strcat(gTlsInfo, num);
                /* name the live entropy source. The old " tsc" arm was a lie
                   the moment tls.c stopped seeding from a TSC-LCG - there is no
                   weak fallback now, so this reads rdrand or jitter. */
                strcat(gTlsInfo, " "); strcat(gTlsInfo, tls_entropy_name());
                gRes[5] = R_OK;
            } else {
                strcpy(gTlsInfo, "err ");
                fmt_u((unsigned)tls_last_error(), num); strcat(gTlsInfo, num);
                gRes[5] = R_FAIL;
            }
            tls_close();
        } else if (rc == TLS_ENOENTROPY) {
            /* refused before the socket: no usable RNG on this machine */
            strcpy(gTlsInfo, "refused: no entropy source");
            gRes[5] = R_FAIL;
        } else { gRes[5] = R_FAIL; }
        gStep = S_DONE;
        break;
    }
    default: break;
    }
}

static void repaint(void);

static void network_tick(void)
{
    int i;
    if (gStep == S_NONIC || gStep == S_DONE) { net_poll(); return; }
    if (!gStarted) { net_poll(); return; }   /* idle until someone asks */
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

/* one "Label   value" line of the summary */
static short kv(short x, short y, const char *k, const char *v)
{
    text_at(x, y, k, C_WHITE, C_BLUE, false);
    text_at((short)(x + 110), y, v && v[0] ? v : "-", C_CYAN, C_BLUE, false);
    return (short)(y + 14);
}

/* the connection, in the words the Control Panel uses for the same facts */
static short summary_draw(short x, short y)
{
    static const char *kSig[5] = { "no signal", "weak signal", "fair signal",
                                   "good signal", "excellent signal" };
    char line[80], ipbuf[20];
    iwl_link_t lk;
    int wired = (gStep != S_NONIC);
    text_at(x, y, "Connection", C_MAG, C_BLUE, false); y += 18;

    if (iwl_present()) {
        iwl_link_info(&lk);
        if (lk.state >= IWL_LINK_NOKEY) {
            int i = 0, j;
            for (j = 0; lk.ssid[j] && i < 40; j++) line[i++] = lk.ssid[j];
            line[i++] = ' '; line[i++] = '-'; line[i++] = ' ';
            { const char *sg = kSig[lk.bars]; for (j = 0; sg[j] && i < 74; j++) line[i++] = sg[j]; }
            line[i] = 0;
        } else if (lk.state == IWL_LINK_JOINING) strcpy(line, "connecting...");
        else                                     strcpy(line, "not connected");
        y = kv(x, y, "Wi-Fi", line);
    } else if (rtwifi_present() || mrvlwifi_present()) {
        y = kv(x, y, "Wi-Fi", "present (no join UI for this card)");
    }
    y = kv(x, y, "Ethernet", !wired ? "no wired adapter"
                                    : net_link() ? "cable in" : "no cable");
    if (net_dhcp_done()) { fmt_ip(ipbuf, net_ip()); y = kv(x, y, "IP address", ipbuf);
                           fmt_ip(ipbuf, net_gw()); y = kv(x, y, "Router", ipbuf); }
    else y = kv(x, y, "IP address", net_link() ? "waiting for an address" : "-");
    if (gWifiMsg[0]) { text_at(x, y, gWifiMsg, C_MAG, C_BLUE, false); y += 16; }
    text_at(x, y, "Wi-Fi networks: Control Panel > Network.", C_WHITE, C_BLUE, false);
    return (short)(y + 18);
}

static void network_draw(UnoWin *w)
{
    Rect r = w->bounds;
    short x = r.left + 10, y = r.top + TBAR_H + 14;
    y = summary_draw(x, y);
    text_at(x, y, "Diagnostics", C_MAG, C_BLUE, false); y += 18;
    if (!gStarted) {
        text_at(x, y, "Tests the stack end to end: DHCP, ping, UDP, TCP, TLS.",
                C_WHITE, C_BLUE, false); y += 14;
        text_at(x, y, "The UDP/TCP/TLS peers only exist under QEMU.",
                C_WHITE, C_BLUE, false); y += 18;
    } else {
        if (gMacStr[0]) { text_at(x, y, "MAC", C_WHITE, C_BLUE, false);
                          text_at(x + 40, y, gMacStr, C_CYAN, C_BLUE, false); y += 14; }
        row(x, y, "Link", gRes[4], 0); y += 14;
        row(x, y, "DHCP lease", gRes[0], gLease); y += 14;
        row(x, y, "Ping gateway", gRes[1], 0); y += 14;
        row(x, y, "UDP (TFTP)", gRes[2], gUdpEcho); y += 14;
        row(x, y, "TCP echo", gRes[3], gTcpEcho); y += 14;
        row(x, y, "TLS (BearSSL)", gRes[5], gTlsInfo); y += 16;
        text_at(x, y, gStep == S_DONE ? "Done." :
                gStep == S_TLS ? "TLS handshake..." : "testing...",
                C_CYAN, C_BLUE, false); y += 4;
    }
    gNetBtn.x = x; gNetBtn.y = (short)(y + 10); gNetBtn.w = 96; gNetBtn.h = 16;
    ui_button(&gNetBtn, gStarted ? "Re-run tests" : "Run tests", false);
}

static void repaint(void)
{
    UnoWin *w = find_app_window(APP_NETWORK);
    if (w) draw_window(w);
}

static void network_click(UnoWin *w, Point p)
{
    if (ui_hit(&gNetBtn, p)) { net_reset(); gStarted = 1; draw_window(w); return; }
}

static Boolean network_key(char ch, short code, Boolean cmd)
{
    (void)code; (void)cmd;
    if (ch == 'r' || ch == 'R') { net_reset(); gStarted = 1; repaint(); return true; }
    return false;
}

/* Opening the app no longer starts a twenty-second test run: the summary above
 * is live either way, and the tests are what someone opens this for on purpose.
 * net_reset() also REBINDS the stack to a wired NIC, which on a machine that
 * had joined WiFi took the network away from the user for opening a window. */
static void network_opened(void)
{
    uno_nic_t *nic = 0;
    if      (e1000_nic())  nic = e1000_nic();
    else if (e1000e_nic()) nic = e1000e_nic();
    else if (igb_nic())    nic = igb_nic();
    else if (r8169_nic())  nic = r8169_nic();
    if (!nic) gStep = S_NONIC;
}

static const AppInterface kIface = {
    network_draw, network_key, network_click, network_tick, network_opened, 0,
    "Network", { 120, 40, 470, 344 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
