/* ===========================================================================
 * UnoDOS/pc64 - the network hardware test (UNO_DEBUG builds only).
 *
 * Runs ONCE from the shell main loop, before the stress driver arms. The plan
 * (arin's spec for the laptop batch rounds):
 *
 *   1. USB Ethernet is not reliably plugged - test it only IF an adapter is
 *      present at boot (the batch adapter is an ASIX AX88179A).
 *   2. If a USB Ethernet adapter IS present, test it and DO NOT test WiFi.
 *   3. Otherwise test WiFi, tracing every bring-up stage - the point is to
 *      see WHERE it dies on each laptop, not just that it died.
 *   4. Neither present (QEMU): fall back to a wired PCI NIC if one exists,
 *      which gives this whole harness a QEMU-verifiable end-to-end path.
 *
 * Everything is logged to CRASH\NETLOG.TXT, flushed line by line so a hang or
 * watchdog reset mid-test still leaves the full trail on disk. Stages are
 * bracketed with uno_dbg_check() so a hang report names the stage, and every
 * trace line feeds the watchdog heartbeat - WiFi bring-up legitimately takes
 * longer than the 20 s watchdog.
 *
 * Credentials: WIFI.CFG or WIFI.TXT at a volume root (ssid= / psk=). The NAS
 * creds template (wifi.txt) rides in via the flasher's developer-options
 * folder copy. STRESS.CFG key `nonet` skips the whole test.
 * ======================================================================== */
#include "uno_debug.h"
#include "net.h"
#include "ax88179.h"
#include "rtl8152.h"
#include "iwlwifi.h"
#include "e1000.h"
#include "e1000e.h"
#include "igb.h"
#include "r8169.h"
#include "usbio.h"
#include "pc64_fs.h"
#include "pc64_pci.h"
#include <string.h>
#include <stdarg.h>

int  vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t cap, const char *fmt, ...);
void uno_pc64_delay_ms(int ms);

/* ---- the NETLOG buffer ---------------------------------------------------- */
#define NETBUF 16384
static char g_buf[NETBUF];
static int  g_len;
static int  g_active;                   /* only flush to disk during the test */

static void flush(void)
{
    if (g_active && g_len > 0)
        uno_dbg_write_crashfile("NETLOG.TXT", g_buf, g_len);
}

void uno_dbg_net_trace(const char *fmt, ...)
{
    char line[240];
    unsigned long long ms = uno_dbg_uptime_ms();
    va_list ap;
    int n;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    uno_dbg_log("%s", line);            /* mirror into the kernel log ring */
    uno_dbg_heartbeat();                /* a slow join must not trip the wd  */
    n = snprintf(g_buf + g_len, (size_t)(NETBUF - g_len - 1), "[%6lu.%03lu] %s\n",
                 (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), line);
    if (n > 0 && g_len + n < NETBUF - 1) g_len += n;
    flush();
}

/* ---- shared IP-layer test (any bound nic) --------------------------------- */
static void pump(int ms)
{
    int i;
    for (i = 0; i < ms / 5; i++) { net_poll(); uno_pc64_delay_ms(5); uno_dbg_heartbeat(); }
}

static const char *ip4(const unsigned char *p, char *tmp)
{
    snprintf(tmp, 16, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
    return tmp;
}

/* Returns 1 if the suite got a lease (link + DHCP), regardless of ping/DNS. */
static int ip_suite(uno_nic_t *nic, const unsigned char *mac, const char *what,
                    int link_wait_ms)
{
    char t[16];
    unsigned long long t0;
    int up = 0, i;

    uno_dbg_net_trace("%s: mac %02x:%02x:%02x:%02x:%02x:%02x", what,
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    net_init(nic, mac);

    uno_dbg_check("net:link");
    t0 = uno_dbg_uptime_ms();
    while ((int)(uno_dbg_uptime_ms() - t0) < link_wait_ms) {
        if (nic->link(nic->ctx)) { up = 1; break; }
        uno_pc64_delay_ms(200);
        uno_dbg_heartbeat();
    }
    uno_dbg_net_trace("%s: link %s after %lu ms", what, up ? "UP" : "still DOWN",
                      (unsigned long)(uno_dbg_uptime_ms() - t0));
    if (!up) { uno_dbg_net_trace("%s: FAIL (no link - cable/AP?)", what); return 0; }

    uno_dbg_check("net:dhcp");
    t0 = uno_dbg_uptime_ms();
    net_dhcp_start();
    while (!net_dhcp_done() && (int)(uno_dbg_uptime_ms() - t0) < 12000) pump(20);
    if (!net_dhcp_done()) {
        uno_dbg_net_trace("%s: FAIL DHCP - no lease in 12 s (link up, so L2 TX/RX "
                          "or the server side is the suspect)", what);
        return 0;
    }
    uno_dbg_net_trace("%s: DHCP lease in %lu ms: ip %s", what,
                      (unsigned long)(uno_dbg_uptime_ms() - t0), ip4(net_ip(), t));
    uno_dbg_net_trace("%s:   gw %s", what, ip4(net_gw(), t));
    uno_dbg_net_trace("%s:   dns %s", what, ip4(net_dns(), t));

    uno_dbg_check("net:ping");
    for (i = 0; i < 3; i++) {
        t0 = uno_dbg_uptime_ms();
        net_ping(net_gw());
        while (!net_ping_replied() && (int)(uno_dbg_uptime_ms() - t0) < 2000) pump(10);
        if (net_ping_replied())
            uno_dbg_net_trace("%s: ping gw #%d reply in %lu ms", what, i + 1,
                              (unsigned long)(uno_dbg_uptime_ms() - t0));
        else
            uno_dbg_net_trace("%s: ping gw #%d NO reply in 2 s", what, i + 1);
    }

    uno_dbg_check("net:dns");
    { unsigned char a[4] = {0,0,0,0};
      if (net_dns_query("example.com", a))
          uno_dbg_net_trace("%s: DNS example.com -> %s", what, ip4(a, t));
      else
          uno_dbg_net_trace("%s: DNS example.com FAILED (resolver %s)", what,
                            ip4(net_dns(), t)); }
    return 1;
}

/* ---- inventory ------------------------------------------------------------ */
static int creds_present(char *name, int cap)
{
    int n = uno_fs_volumes(), i;
    for (i = 0; i < n; i++) {
        if (uno_fs_size(i, "WIFI.CFG") > 0) { snprintf(name, (size_t)cap, "WIFI.CFG (vol %d)", i); return 1; }
        if (uno_fs_size(i, "WIFI.TXT") > 0) { snprintf(name, (size_t)cap, "WIFI.TXT (vol %d)", i); return 1; }
    }
    return 0;
}

static void inventory(void)
{
    int n, i;
    pci_dev d;
    uno_dbg_check("net:inventory");
    uno_dbg_net_trace("== network hardware test, build %s, machine %s ==",
                      uno_dbg_build_id(), uno_dbg_machine_tag());

    n = uno_usbio_count();
    uno_dbg_net_trace("usb (firmware UsbIo): %d interface handle(s)", n);
    for (i = 0; i < n; i++) {
        unsigned short v = 0, p = 0;
        unsigned char c = 0, s = 0;
        if (uno_usbio_info(i, &v, &p, &c, &s) == 0)
            uno_dbg_net_trace("  usb[%d] %04x:%04x class %02x/%02x", i, v, p, c, s);
    }
    if (pci_find_class(0x02, 0x00, &d))
        uno_dbg_net_trace("pci: wired NIC %04x:%04x at %d/%d/%d",
                          d.vendor, d.device, d.bus, d.dev, d.fn);
    if (pci_find_class(0x02, 0x80, &d))
        uno_dbg_net_trace("pci: net-other (WiFi?) %04x:%04x at %d/%d/%d",
                          d.vendor, d.device, d.bus, d.dev, d.fn);
}

/* ---- the one-shot test ----------------------------------------------------- */
static void run_test(void)
{
    unsigned short vid = 0, pid = 0;
    uno_nic_t *nic;

    inventory();

    /* 1) USB Ethernet, only if present - and if present, WiFi is NOT tested
     *    (arin: adapters come and go; a plugged adapter means "eth round"). */
    if (ax88179_present(&vid, &pid) || rtl8152_present(&vid, &pid)) {
        uno_dbg_net_trace("plan: USB ethernet adapter %04x:%04x present -> "
                          "eth test, WiFi SKIPPED", vid, pid);
        uno_dbg_check("net:eth-bind");
        { const unsigned char *mac = 0;
          nic = ax88179_nic();
          if (nic) mac = ax88179_mac();
          else { nic = rtl8152_nic(); if (nic) mac = rtl8152_mac(); }
          if (!nic) {
              uno_dbg_net_trace("eth: FAIL adapter present but no driver bound "
                                "(see trace above for the decline reason)");
              uno_dbg_net_trace("== net test done: ETH FAIL (bind) ==");
              return;
          }
          { int ok = ip_suite(nic, mac, "eth", 8000);
            uno_dbg_net_trace("== net test done: ETH %s ==", ok ? "PASS" : "FAIL"); } }
        return;
    }

    /* 2) WiFi. Pre-flight the ingredients first so a missing prerequisite is
     *    one obvious line, then run the full traced bring-up. */
    {
        char cn[32];
        int have_creds = creds_present(cn, sizeof cn);
        pci_dev d;
        int have_card = pci_find_class(0x02, 0x80, &d) && d.vendor == 0x8086;
        uno_dbg_net_trace("plan: no USB ethernet -> WiFi test (card:%s creds:%s)",
                          have_card ? "yes" : "NO", have_creds ? cn : "MISSING");
        if (have_card || have_creds) {
            char st[192];
            uno_dbg_check("net:wifi-bringup");
            nic = iwl_nic();
            iwl_status_str(st, sizeof st);
            uno_dbg_net_trace("wifi: driver status: %s", st);
            if (nic) {
                int ok = ip_suite(nic, iwl_mac(), "wifi", 15000);
                uno_dbg_net_trace("== net test done: WIFI %s ==", ok ? "PASS" : "FAIL");
            } else {
                uno_dbg_net_trace("== net test done: WIFI FAIL (bring-up stopped, "
                                  "see the FAIL line above) ==");
            }
            return;
        }
    }

    /* 3) Nothing USB, nothing Intel-WiFi: a wired PCI NIC if the machine has
     *    one. This is the QEMU path (e1000), so the whole harness stays
     *    regression-testable without WiFi silicon. */
    uno_dbg_check("net:wired");
    { const unsigned char *mac = 0;
      const char *name = 0;
      nic = e1000_nic();  if (nic) { mac = e1000_mac();  name = "e1000";  }
      if (!nic) { nic = e1000e_nic(); if (nic) { mac = e1000e_mac(); name = "e1000e"; } }
      if (!nic) { nic = igb_nic();    if (nic) { mac = igb_mac();    name = "igb";    } }
      if (!nic) { nic = r8169_nic();  if (nic) { mac = r8169_mac();  name = "r8169";  } }
      if (!nic) {
          uno_dbg_net_trace("no testable network hardware at all (no USB eth, "
                            "no Intel WiFi, no wired PCI NIC)");
          uno_dbg_net_trace("== net test done: NOTHING TO TEST ==");
          return;
      }
      uno_dbg_net_trace("plan: wired PCI NIC via %s", name);
      { int ok = ip_suite(nic, mac, name, 5000);
        uno_dbg_net_trace("== net test done: WIRED %s ==", ok ? "PASS" : "FAIL"); } }
}

void pc64_nettest_tick(void)
{
    static int done, frames;
    int flag;
    if (done) return;
    if (++frames < 30) return;           /* let the desktop paint first (~0.5 s) */
    done = 1;                            /* one shot, whatever happens below */
    flag = pc64_stress_cfg_flag("nonet");
    if (flag < 0) return;                /* no STRESS.CFG = not a test stick  */
    if (flag > 0) { uno_dbg_log("net: skipped (nonet in STRESS.CFG)"); return; }
    g_active = 1;
    run_test();
    flush();
    g_active = 0;
}
