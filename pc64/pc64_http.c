/* pc64_http - HTTP/1.0 GET for the browser. See pc64_http.h. */
#include "pc64_http.h"
#include "net.h"
#include "e1000.h"
#include "ax88179.h"      /* USB Ethernet fallback (X1 has no wired NIC) */
#include "tls.h"          /* tls_connect_ca / tls_write / tls_read (HTTPS) */
#include <string.h>

void uno_pc64_delay_ms(int ms);          /* firmware Stall (uefi_main) */

/* ---- shared network bring-up (idempotent) -------------------------------- */
static int g_net_inited;

int pc64_net_up(void)
{
    uno_nic_t *nic;
    const unsigned char *mac;
    if (g_net_inited) return net_link() || 1;   /* already up (link may flap) */
    nic = e1000_nic(); mac = e1000_mac();       /* native PCI NIC first */
    if (!nic) { nic = ax88179_nic(); mac = ax88179_mac(); }   /* else a USB Ethernet adapter */
    if (!nic) return 0;                          /* no NIC at all */
    net_init(nic, mac);
    g_net_inited = 1;
    net_dhcp_start();
    { int i; for (i = 0; i < 400 && !net_dhcp_done(); i++) { net_poll(); uno_pc64_delay_ms(5); } }
    return 1;
}

/* ---- tiny helpers -------------------------------------------------------- */
static void set_tls_err(char *status, int statusmax, const char *what)
{
    int e = tls_last_error(), i = 0, n = 0; char num[8];
    if (statusmax <= 0) return;
    while (what[i] && i < statusmax-1) { status[i] = what[i]; i++; }
    { const char *sfx = " (BearSSL err "; int k = 0; while (sfx[k] && i < statusmax-1) status[i++] = sfx[k++]; }
    if (e < 0) { if (i < statusmax-1) status[i++]='-'; e = -e; }
    if (!e) num[n++]='0'; while (e) { num[n++]=(char)('0'+e%10); e/=10; }
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

    if (!pc64_net_up()) { if (statusmax) strncpy(status,"No e1000 NIC found (Wi-Fi not supported)",statusmax-1); return -3; }

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
