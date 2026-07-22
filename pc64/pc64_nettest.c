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
#include "unoauto.h"
#include "unoauto_remote.h"
#include "fb.h"
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
void uno_pc64_present(void);            /* fb -> panel (uefi_main)            */
void pc64_dbg_mark_dirty(void);         /* full shell repaint next frame      */

/* ---- boot-test progress banner (see uno_debug.h) --------------------------
 * Painted synchronously from inside the blocking test phase, because the
 * shell loop that would normally repaint is the thing being blocked. A
 * full-width strip so a shorter message always erases a longer one; presented
 * immediately (the dirty-span present makes a one-strip paint cheap). */
static int g_prog_shown;

void uno_dbg_progress(const char *fmt, ...)
{
    char msg[128];
    va_list ap;
    int n, sh = 20;
    if (uno_fb_w <= 0) return;             /* fb not up yet */
    n = snprintf(msg, sizeof msg, " BOOT TESTS   ");
    va_start(ap, fmt);
    vsnprintf(msg + n, sizeof msg - (size_t)n, fmt, ap);
    va_end(ap);
    fb_fill_rect(0, 0, uno_fb_w, sh, FB_RGB(52, 40, 10));
    fb_hline(0, sh - 1, uno_fb_w, FB_RGB(120, 92, 26));
    fb_text(6, 3, msg, FB_RGB(255, 214, 96), -1);
    g_prog_shown = 1;
    uno_pc64_present();
}

void uno_dbg_progress_done(void)
{
    if (!g_prog_shown) return;
    g_prog_shown = 0;
    pc64_dbg_mark_dirty();                 /* shell repaints over the strip */
}

/* ---- the NETLOG buffer: an unoauto NET-channel sink ------------------------
 * unoauto_log(UA_CH_NET, ...) is the trace path now; uno_dbg_net_trace below
 * is a compatibility wrapper over it, so every existing driver call site is
 * unchanged.  unoauto's sink 0 (the kernel-ring mirror) runs first, then this
 * sink does what the old trace body did after the mirror: feed the watchdog,
 * paint the ticker, append the timestamped line, flush - same order as before
 * the seam, so NETLOG.TXT is byte-identical. */
#define NETBUF 16384
static char g_buf[NETBUF];
static int  g_len;
static int  g_active;                   /* only flush to disk during the test */

static void flush(void)
{
    if (g_active && g_len > 0)
        uno_dbg_write_crashfile("NETLOG.TXT", g_buf, g_len);
}

static void netlog_sink(UnoAutoChan ch, const char *line, void *user)
{
    unsigned long long ms = uno_dbg_uptime_ms();
    int n;
    (void)ch; (void)user;
    uno_dbg_heartbeat();                /* a slow join must not trip the wd  */
    uno_dbg_progress("%s", line);       /* live on-screen ticker (blocking phase) */
    n = snprintf(g_buf + g_len, (size_t)(NETBUF - g_len - 1), "[%6lu.%03lu] %s\n",
                 (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), line);
    if (n > 0 && g_len + n < NETBUF - 1) g_len += n;
    flush();
}

/* Called by unoauto's ua_init (transitional, see unoauto.c) and by the
 * wrapper below, whichever runs first. */
void pc64_netlog_sink_ensure(void)
{
    static int did;
    if (did) return;
    did = 1;
    unoauto_sink_add(1u << UA_CH_NET, netlog_sink, 0);
}

void uno_dbg_net_trace(const char *fmt, ...)
{
    va_list ap;
    pc64_netlog_sink_ensure();
    va_start(ap, fmt);
    unoauto_vlog(UA_CH_NET, fmt, ap);
    va_end(ap);
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
        /* Frame counters turn the old three-way guess into an answer:
         *   tx==0            -> our TX path never fired (driver/xHCI bulk-out)
         *   tx>0, rx==0      -> nothing came back: our RX parse, the cable, or
         *                       a silent DHCP server
         *   rx>0, ip==0      -> frames arrive (ARP) but no IP: RX filter/offset
         *   rx>0, ip>0       -> we saw IP but no lease: DHCP option parsing      */
        uno_dbg_net_trace("%s: FAIL DHCP - no lease in 12 s "
                          "(tx=%lu rx=%lu arp=%lu ip=%lu)", what,
                          (unsigned long)net_tx_frames(), (unsigned long)net_rx_frames(),
                          (unsigned long)net_rx_arp(),    (unsigned long)net_rx_ip());
        if (net_tx_frames() == 0)
            uno_dbg_net_trace("%s:   -> TX never fired: driver send / bulk-out path", what);
        else if (net_rx_frames() == 0)
            uno_dbg_net_trace("%s:   -> TX ok, nothing received: RX parse, cable, or dead server", what);
        else if (net_rx_ip() == 0)
            uno_dbg_net_trace("%s:   -> frames arrive but no IP: RX filter / descriptor offset", what);
        else
            uno_dbg_net_trace("%s:   -> IP seen, no lease: DHCP offer/option parsing", what);
        return 0;
    }
    uno_dbg_net_trace("%s: DHCP lease in %lu ms: ip %s", what,
                      (unsigned long)(uno_dbg_uptime_ms() - t0), ip4(net_ip(), t));
    uno_dbg_net_trace("%s:   gw %s", what, ip4(net_gw(), t));
    uno_dbg_net_trace("%s:   dns %s", what, ip4(net_dns(), t));
    /* DHCP-ACK diagnostics: if dns shows the SLIRP default (10.0.2.3), had_dns=0
     * says option 6 wasn't in the ACK we parsed. A short ack_len with had_rtr=1
     * (opt 3 present) but had_dns=0 means our RX truncated the ACK after opt 3 -
     * the AX88179 RX-length suspect; a full ack_len means the router omitted it. */
    uno_dbg_net_trace("%s:   dhcp-ack: len=%d opt3(rtr)=%d opt6(dns)=%d", what,
                      net_dhcp_ack_len(), net_dhcp_had_rtr(), net_dhcp_had_dns());

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
    /* target changed example.com -> api.anthropic.com: the lab's Bell router
     * (mynetwork.home) returns NXDOMAIN for example.com SPECIFICALLY (verified
     * from a Windows host with the same minimal query - google/cloudflare/
     * anthropic all resolve). The stack was right; the name was filtered.
     * api.anthropic.com is also exactly what the AI checks resolve. */
    { unsigned char a[4] = {0,0,0,0};
      if (net_dns_query("api.anthropic.com", a))
          uno_dbg_net_trace("%s: DNS api.anthropic.com -> %s", what, ip4(a, t));
      else {
          uno_dbg_net_trace("%s: DNS api.anthropic.com FAILED (resolver %s)", what,
                            ip4(net_dns(), t));
          uno_dbg_net_trace("%s:   dns-diag: sent=%d rx=%d badid=%d neg=%d "
                            "(rx=0 no reply ever arrived; neg>0 forwarder answered negatively)",
                            what, net_dns_sent(), net_dns_rx(), net_dns_badid(), net_dns_neg());
      } }
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
    /* Flasher test-selection overrides (STRESS.CFG):
     *   net-force-wifi : test WiFi even when a USB ethernet adapter is present
     *   net-eth-only   : test ONLY ethernet (no WiFi fallback)
     * Neither = the default auto-detect (adapter present -> eth, else WiFi). */
    int force_wifi = pc64_stress_cfg_flag("net-force-wifi") > 0;
    int eth_only   = pc64_stress_cfg_flag("net-eth-only") > 0;

    inventory();
    if (force_wifi) uno_dbg_net_trace("plan: net-force-wifi set - testing WiFi "
                                      "regardless of any USB ethernet adapter");
    if (eth_only)   uno_dbg_net_trace("plan: net-eth-only set - ethernet only, "
                                      "no WiFi fallback");

    /* 1) USB Ethernet - the default when an adapter is present (a plugged
     *    adapter means "eth round"), unless net-force-wifi says skip to WiFi. */
    if (!force_wifi && (ax88179_present(&vid, &pid) || rtl8152_present(&vid, &pid))) {
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

    if (eth_only) {
        uno_dbg_net_trace("eth: no USB ethernet adapter present (net-eth-only, "
                          "so no WiFi fallback)");
        uno_dbg_net_trace("== net test done: ETH SKIPPED (no adapter) ==");
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

void uno_pc64_shutdown(void);            /* platform: power off (marks clean) */

/* End the one-shot test phase: hand the progress banner back, then power the
 * machine off IF this is an unattended/headless test stick that asked for it.
 * That auto-poweroff used to be a side effect of the stress driver's `passes=N`
 * run; the stress driver was removed, so the one-shot spec/net path owns it now
 * - without this a QEMU harness (which sets `passes=1`) would boot, run the
 * suites, and hang forever waiting for a power-off that never came.
 *   poweroff        explicit "shut down when the suites finish"
 *   passes= / once  legacy poweroff signal (older harness/flasher configs)
 *   noshutdown      veto - leave the desktop up (operator-present metal runs)
 * A stick with no STRESS.CFG (all flags < 0) never powers off - a normal boot. */
static void nettest_finish(void)
{
    uno_dbg_progress_done();
    if (pc64_stress_cfg_flag("noshutdown") <= 0 &&
        (pc64_stress_cfg_flag("poweroff") > 0 ||
         pc64_stress_cfg_flag("passes")   > 0 ||
         pc64_stress_cfg_flag("once")     > 0)) {
        uno_dbg_log("nettest: one-shot suites done - powering off");
        uno_dbg_write_bootlog();         /* final kernel-log flush while FS alive */
        uno_pc64_shutdown();             /* marks clean + powers off; no return */
    }
}

/* ---- unoautomate boot runner ----------------------------------------------
 * STRESS.CFG key `automate` + an AUTOMATE.PY at any volume root = run it as
 * a Python app once the boot-test phase ends.  The script's tick() drives
 * the automation (see upy_port/mod_unoauto.c); it powers the machine off
 * itself via unoauto.poweroff() when it finishes an unattended run. */
int pc64_shell_run_py(int vol, const char *path);      /* pc64_uui.c (debug) */
const char *pc64_shell_py_error(void);

static void automate_start(void)
{
    int i, n;
    /* Arm the remote dev-PC link now that the boot net test has released the
     * single TCP connection.  No-op unless STRESS.CFG has a `remote=` key. */
    unoauto_remote_boot();
    if (pc64_stress_cfg_flag("automate") <= 0) return;
    n = uno_fs_volumes();
    for (i = 0; i < n; i++)
        if (uno_fs_size(i, "AUTOMATE.PY") > 0) {
            int rc;
            uno_dbg_log("unoauto: running AUTOMATE.PY (vol %d)", i);
            rc = pc64_shell_run_py(i, "AUTOMATE.PY");
            if (rc != 0)
                uno_dbg_log("unoauto: AUTOMATE.PY failed (%d): %s",
                            rc, pc64_shell_py_error());
            return;
        }
    uno_dbg_log("unoauto: automate set but no AUTOMATE.PY on any volume");
}

void pc64_nettest_tick(void)
{
    static int done, frames;
    int flag;
    if (done) return;
    if (++frames < 30) return;           /* let the desktop paint first (~0.5 s) */
    done = 1;                            /* one shot, whatever happens below */
    /* P3 opt-in: flip the framebuffer to write-combining if the operator asked
     * (mtrr-wc). Runs before the net test so the whole session benefits; it is
     * self-reverting when it can't prove a win, and refuses on any geometry it
     * can't tile safely. */
    if (pc64_stress_cfg_flag("mtrr-wc") > 0) {
        g_active = 1;
        uno_dbg_net_trace("== P3 MTRR write-combining experiment (operator-opted) ==");
        uno_pc64_mtrr_wc_experiment();
        flush();
        g_active = 0;
    }
    /* SPECTEST conformance suite (spec key), before the net test + stress */
    if (pc64_stress_cfg_flag("spec") > 0)
        pc64_spectest_run();
    flag = pc64_stress_cfg_flag("nonet");
    if (flag < 0) { uno_dbg_progress_done(); automate_start(); return; }
                                         /* not a test stick (but automation
                                            may still be configured)          */
    if (flag > 0) { uno_dbg_log("net: skipped (nonet in STRESS.CFG)");
                    nettest_finish(); automate_start(); return; }
    g_active = 1;
    run_test();
    flush();
    g_active = 0;
    nettest_finish();                    /* banner back, then power off if asked */
    automate_start();                    /* unreached if nettest_finish powered off */
}
