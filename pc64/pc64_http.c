/* pc64_http - HTTP/1.0 GET for the browser. See pc64_http.h. */
#include "pc64_http.h"
#include "net.h"
#include "e1000.h"
#include "e1000e.h"       /* Intel e1000e GbE (82574 / I217/I218/I219 LOM) */
#include "igb.h"          /* Intel igb GbE (I210/I211/82576) */
#include "r8169.h"        /* Realtek RTL8168/8111 GbE (the common consumer NIC) */
#include "ax88179.h"      /* USB Ethernet fallback (X1 has no wired NIC) */
#include "rtl8152.h"      /* the other common USB Ethernet family (docks) */
#include "iwlwifi.h"      /* Intel AC/AX WiFi (firmware-driven; WIFI.CFG) */
#include "rtwifi.h"       /* Realtek PCIe WiFi (rtw88/rtw89) */
#include "mrvlwifi.h"     /* Marvell/NXP PCIe WiFi (mwifiex) */
#include "tls.h"          /* tls_connect_ca / tls_write / tls_read (HTTPS) */
#include <string.h>

void uno_pc64_delay_ms(int ms);          /* firmware Stall (uefi_main) */

/* ---- shared network bring-up (idempotent) -------------------------------- */
static int g_net_inited;

struct nicent { uno_nic_t *(*n)(void); const unsigned char *(*m)(void); };
/* wired NICs are cheap to probe; the WiFi entries run a full multi-second
 * bring-up ON PROBE, so they are a separate tier reached only if no wired NIC
 * exists at all. */
static const struct nicent g_wired[] = {
    { e1000_nic, e1000_mac },   /* native PCI (82540/82545)      */
    { e1000e_nic, e1000e_mac }, /* Intel e1000e (82574 / I219)   */
    { igb_nic, igb_mac },       /* Intel igb (I210/I211/82576)   */
    { r8169_nic, r8169_mac },   /* Realtek RTL8168/8111          */
    { ax88179_nic, ax88179_mac },/* USB Ethernet (ASIX)          */
    { rtl8152_nic, rtl8152_mac },/* USB Ethernet (Realtek dock)  */
};
static const struct nicent g_wifi[] = {
    { iwl_nic, iwl_mac }, { rtwifi_nic, rtwifi_mac }, { mrvlwifi_nic, mrvlwifi_mac },
};

/* run DHCP on the already-net_init'd nic, waiting for link first. USB ethernet
 * (ax88179/rtl8152) programs the MAC medium from the PHY's negotiated status
 * inside nic->link(), and a wrong medium silently kills RX, so link MUST be up
 * before DHCP. ~3 s covers autoneg. */
static int net_dhcp_after_link(uno_nic_t *nic)
{
    int i;
    if (nic->link) { for (i = 0; i < 600 && !nic->link(nic->ctx); i++) uno_pc64_delay_ms(5); }
    /* net_dhcp_start sends one DISCOVER; net_poll's dhcp_tick retransmits it
     * (and the REQUEST) every ~1.5 s, so a lost OFFER/ACK recovers. Wait ~9 s. */
    net_dhcp_start();
    for (i = 0; i < 1800 && !net_dhcp_done(); i++) { net_poll(); uno_pc64_delay_ms(5); }
    return 1;
}

int pc64_net_up(void)
{
    uno_nic_t *nic, *fb = 0;
    const unsigned char *fbmac = 0;
    int i, nw = (int)(sizeof g_wired / sizeof g_wired[0]);
    if (g_net_inited) return net_link() || 1;   /* already up (link may flap) */

    /* Reuse a stack that already holds a DHCP lease. The boot eth TEST brings
     * the ASIX up and leases with WORKING RX; re-net_init'ing it here starts a
     * second bring-up whose RX comes up dead (tx>0 rx=0 - the medium/bulk-in
     * re-arm does not survive the re-init), stranding the remote link on a
     * link-up-but-leaseless NIC. Gating on an actual lease (not just link) is
     * safe: the null test NIC used by S-NET-08/19 never leases, so SPECTEST
     * still exercises the full fresh-probe path below. */
    if (net_dhcp_done()) { g_net_inited = 1; return 1; }

    /* Pass 1: a WIRED NIC that reports link up wins. This is the fix for a
     * machine with a cableless onboard Intel port (e.g. the X13 Yoga's I219
     * 0x0d4f, claimed by e1000e) sitting IN FRONT of the USB dongle that
     * actually has the cable - committing to the linkless onboard port left
     * the net stack "bound, no link" and the remote link never dialed. */
    for (i = 0; i < nw; i++) {
        nic = g_wired[i].n(); if (!nic) continue;
        if (!fb) { fb = nic; fbmac = g_wired[i].m(); }   /* first bound = fallback */
        if (nic->link && nic->link(nic->ctx)) {          /* has a cable */
            net_init(nic, g_wired[i].m()); g_net_inited = 1;
            return net_dhcp_after_link(nic);
        }
    }
    /* Pass 2: a wired NIC bound but none had link yet - use the first and wait
     * for autoneg (a cable that just needs a moment). */
    if (fb) { net_init(fb, fbmac); g_net_inited = 1; return net_dhcp_after_link(fb); }
    /* Pass 3: no wired NIC at all - fall back to WiFi (expensive bring-up). */
    for (i = 0; i < (int)(sizeof g_wifi / sizeof g_wifi[0]); i++) {
        nic = g_wifi[i].n();
        if (nic) { net_init(nic, g_wifi[i].m()); g_net_inited = 1; return net_dhcp_after_link(nic); }
    }
    return 0;                                    /* no NIC at all */
}

/* ---- tiny helpers -------------------------------------------------------- */
static void set_tls_err(char *status, int statusmax, const char *what)
{
    int e = tls_last_error(), i = 0, n = 0; char num[8];
    if (statusmax <= 0) return;
    while (what[i] && i < statusmax-1) { status[i] = what[i]; i++; }
    { const char *sfx = " (BearSSL err "; int k = 0; while (sfx[k] && i < statusmax-1) status[i++] = sfx[k++]; }
    if (e < 0) { if (i < statusmax-1) status[i++]='-'; e = -e; }
    if (!e) num[n++]='0'; while (e && n < (int)sizeof num) { num[n++]=(char)('0'+e%10); e/=10; }
    while (n && i < statusmax-1) status[i++] = num[--n];
    if (i < statusmax-1) status[i++]=')';
    status[i] = 0;
}

static int is_ipv4(const char *s, unsigned char out[4])
{
    int part = 0, val = 0, digits = 0, i;
    for (i = 0; ; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') { val = val*10 + (c-'0'); digits++; if (val > 255) return 0; }
        else if (c == '.' || c == 0) {
            if (!digits || part > 3) return 0;
            out[part++] = (unsigned char)val; val = 0; digits = 0;
            if (c == 0) break;
        } else return 0;
    }
    return part == 4;
}

/* ---- GET ----------------------------------------------------------------- */
int pc64_http_get(const char *url, char *body, int bodymax, char *status, int statusmax)
{
    char host[128], path[512];
    unsigned char ip[4];
    int port = 80, hn = 0, i, secure = 0;
    const char *p = url;

    if (statusmax > 0) status[0] = 0;
    if (bodymax > 0) body[0] = 0;

    /* scheme */
    if (!strncmp(p, "https://", 8)) { secure = 1; port = 443; p += 8; }
    else if (!strncmp(p, "http://", 7)) p += 7;

    /* host[:port] */
    while (*p && *p != '/' && *p != ':' && hn < (int)sizeof(host)-1) host[hn++] = *p++;
    host[hn] = 0;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port*10 + (*p++ - '0'); }
    /* path */
    if (*p != '/') { path[0] = '/'; i = 1; } else { i = 0; }
    { int j = i; while (*p && j < (int)sizeof(path)-1) path[j++] = *p++; path[j] = 0; if (!i && !path[0]) strcpy(path,"/"); }
    if (path[0] == 0) strcpy(path, "/");
    if (host[0] == 0) { if (statusmax) strncpy(status,"Empty host",statusmax-1); return -2; }

    if (!pc64_net_up()) { if (statusmax) strncpy(status,"No network link (no NIC, or WiFi not joined - check WIFI.CFG + firmware)",statusmax-1); return -3; }

    /* resolve */
    if (!is_ipv4(host, ip)) {
        if (!net_dns_query(host, ip)) { if (statusmax) strncpy(status,"DNS lookup failed",statusmax-1); return -4; }
    }

    /* connect (plain TCP, or TLS with CA validation for https) */
    if (secure) {
        int rc = tls_connect_ca(ip, (unsigned short)port, host);
        if (rc != 0) { set_tls_err(status, statusmax, "TLS connect failed"); return -5; }
    } else {
        if (net_tcp_connect(ip, (unsigned short)port) < 0) { if (statusmax) strncpy(status,"TCP connect failed",statusmax-1); return -5; }
        for (i = 0; i < 400 && net_tcp_state() == TCP_SYN_SENT; i++) { net_poll(); uno_pc64_delay_ms(5); }
        if (net_tcp_state() != TCP_ESTABLISHED) { net_tcp_close(); if (statusmax) strncpy(status,"Connection timed out",statusmax-1); return -6; }
    }

    /* request */
    { char req[1024]; int rn = 0;
      const char *a = "GET ", *b = " HTTP/1.0\r\nHost: ", *c = "\r\nUser-Agent: UnoDOS-pc64\r\nConnection: close\r\nAccept: text/html,text/markdown,text/plain\r\n\r\n";
      /* every append is bounds-checked against sizeof(req): host/path come from
         the address bar AND from links in untrusted pages, so a crafted long URL
         must not overflow this stack buffer. */
      #define REQ_PUT(s) do { int l=(int)strlen(s); if (rn+l >= (int)sizeof(req)) { if (statusmax) strncpy(status,"URL too long",statusmax-1); if (secure) tls_close(); else net_tcp_close(); return -8; } memcpy(req+rn,(s),l); rn+=l; } while (0)
      REQ_PUT(a); REQ_PUT(path); REQ_PUT(b); REQ_PUT(host); REQ_PUT(c);
      #undef REQ_PUT
      if (secure) { if (tls_write(req, rn) < 0) { set_tls_err(status, statusmax, "TLS write failed"); tls_close(); return -7; } }
      else        net_tcp_send(req, rn);
    }

    /* receive until the server closes (Connection: close) or we time out */
    { static char raw[49152];
      int rn = 0, idle = 0;
      while (rn < (int)sizeof(raw)-1) {
          char tmp[1460]; int n;
          if (secure) {
              n = tls_read(tmp, sizeof tmp);
              if (n > 0) { if (rn+n > (int)sizeof(raw)-1) n = (int)sizeof(raw)-1-rn; memcpy(raw+rn,tmp,n); rn += n; }
              else break;                        /* <=0: TLS closed or error */
          } else {
              net_poll();
              n = net_tcp_recv(tmp, sizeof tmp);
              if (n > 0) { if (rn+n > (int)sizeof(raw)-1) n = (int)sizeof(raw)-1-rn; memcpy(raw+rn,tmp,n); rn += n; idle = 0; }
              else {
                  int st = net_tcp_state();
                  if (st == TCP_DONE || st == TCP_CLOSED || st == TCP_FIN_WAIT) { if (++idle > 20) break; }
                  else if (++idle > 600) break;  /* ~3s of no data */
                  uno_pc64_delay_ms(5);
              }
          }
      }
      raw[rn] = 0;
      if (secure) tls_close(); else net_tcp_close();

      /* status line */
      if (statusmax > 0) { int j=0; const char *s=raw; while (*s && *s!='\r' && *s!='\n' && j<statusmax-1) status[j++]=*s++; status[j]=0;
                           if (j==0) strncpy(status,"No response",statusmax-1); }

      /* split headers from body at the blank line */
      { const char *bp = raw, *e = raw + rn; const char *split = 0;
        for (; bp + 3 < e; bp++) {
            if (bp[0]=='\r'&&bp[1]=='\n'&&bp[2]=='\r'&&bp[3]=='\n') { split = bp+4; break; }
            if (bp[0]=='\n'&&bp[1]=='\n') { split = bp+2; break; }
        }
        if (!split) split = raw;                 /* no headers found - show all */
        { int bl = (int)(e - split); if (bl > bodymax-1) bl = bodymax-1; if (bl < 0) bl = 0;
          memcpy(body, split, bl); body[bl] = 0; return bl; }
      }
    }
}
