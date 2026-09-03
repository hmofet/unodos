/* cosmo64/urc.c -- the unoautomate remote channel (URC) on the Cosmo (M6).
 *
 * pc64's URC subsystem -- unoauto.c (LOG/TEST/HOOK), unoauto_probe.c,
 * unoauto_gate.c, unoauto_remote.c (the line protocol and its verbs),
 * unoauto_screen.c (QOI screen grabs), netdisc.c (LAN discovery) -- compiles
 * for this platform UNCHANGED (probe 2026-09-02: every file, zero edits).
 * What it reaches for that this platform did not have is small, and it all
 * lives here:
 *
 *  1. THE DEBUG.CFG READER. pc64_stress.c reads keys off a FAT volume; this
 *     device mounts none, so the keys are answered from what the platform
 *     knows about itself. On the Cosmo: `listen` -- the box is a URC SERVER
 *     on :5099 and the dev PC dials in, the shape a box with a moving DHCP
 *     lease wants. Under the QEMU gate (no USB, so no NIC): `remote-serial`
 *     -- URC over the virt board's PL011, which is what makes the whole
 *     dispatcher gate-able without hardware. Optionally `urc-auth=<PIN>`,
 *     see WHICH GATE below.
 *
 *  2. THE PRODUCTION FALLBACKS unoauto_compat.c would supply (the kernel log
 *     ring, uptime, the guard, the profiler). That file is deliberately NOT
 *     compiled here: its uno_dbg_log drops every line, and its uptime is
 *     mac_compat's TickCount(), which is a CALL COUNTER, not a clock. Here
 *     every unoauto LOG line reaches the eMMC log, and uptime is CNTPCT.
 *
 *  3. THE SYMBOLS THE VERBS NAME THAT THIS MACHINE CANNOT HAVE: UEFI boot
 *     entries, the iwlwifi debug hook, RDRAND entropy. Each answers "absent"
 *     in the way its caller reports, never 0-as-success.
 *
 *  4. THE SERIAL TRANSPORT (uart_init/write/read, unoauto_serial.h) on the
 *     PL011 at 0x09000000 that QEMU's virt board carries. unoauto_serial.c
 *     is a 16550 driven by x86 port I/O and does not compile here; the seam
 *     above it is three functions, so this is the QEMU gate's wire.
 *
 * WHICH GATE, AND WHY NOT THE PRODUCTION ONE. In production the channel stays
 * disarmed until a console user arms it (unoauto_gate.h), and arming needs a
 * bound unosecure session = an account = a persistent store on a FAT volume.
 * This device has no volume yet (the SD card is a later milestone), so the
 * production arming path is unreachable here today. Instead unoauto_gate.c
 * and unoauto_remote.c are compiled -DUNO_DEBUG -- per file, the same trick
 * as the usb renames; the headers they share with the rest of the build carry
 * no UNO_DEBUG-conditional layouts -- so the gate is open the way it is on a
 * debug stick: the channel comes up on its own and, with no `urc-auth`, every
 * verb is allowed to anyone who can reach :5099. That is a dev device on a
 * home LAN. To close it, build with URC_PIN=123456 ./build.sh shell: the
 * gate then runs the PRODUCTION rules with that token (unoauto_gate_boot's
 * urc-auth hook), granting OBSERVE and DRIVE only -- every SYSTEM verb (put,
 * mkfs, reboot, py, ...) is refused, and three bad tokens stand the channel
 * down for the boot.
 *
 * WHO BRINGS IT UP. On x86 unoauto_remote_boot() is called from the debug
 * net test, from the arming panel, or from the shell only when accounts
 * exist. None of those paths exist here, so c64_urc_tick() (pumped from
 * uno_pc64_poll) calls it once, the frame after netup.c's bring-up has run:
 * the listen transport needs an address to bind, and the serial one needs
 * nothing at all.
 */

#include "cosmo64.h"

#if __has_include("urc_pin.h")
#include "urc_pin.h"                 /* build.sh writes: #define C64_URC_PIN "..." */
#endif

/* ---- which board is this? ------------------------------------------------ */
/* Read once off the DTB LK (or QEMU) handed over: the virt board's root node
 * is compatible = "linux,dummy-virt". Everything that differs between the
 * gate and the device hangs off this one fact. */
static int g_virt = -1;

static int on_virt(void)
{
    if (g_virt < 0)
        g_virt = c64_fdt_root_compat_has((const void *)FBDBG->dtb_ptr,
                                         "linux,dummy-virt");
    return g_virt;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- 1. the DEBUG.CFG reader --------------------------------------------- */
/* Contract (pc64_stress.c): flag() > 0 = the key is set, 0 = absent;
 * value() returns the value's length, 0 = absent or bare. Everything not
 * named here is absent, which is what keeps the rest of unoauto's boot-time
 * behaviour (automate scripts, dial-out, discovery) off. */
int pc64_stress_cfg_flag(const char *key)
{
    if (streq(key, "listen"))
        return on_virt() ? 0 : 1;            /* the device: a URC server    */
    if (streq(key, "remote-serial"))
        return on_virt() ? 1 : 0;            /* the gate: URC over PL011    */
#ifdef C64_URC_PIN
    if (streq(key, "urc-auth"))
        return 1;
#endif
    return 0;
}

int pc64_stress_cfg_value(const char *key, char *buf, int cap)
{
    if (buf && cap > 0)
        buf[0] = 0;
#ifdef C64_URC_PIN
    if (streq(key, "urc-auth") && buf) {
        int n = 0;
        while (C64_URC_PIN[n] && n < cap - 1) {
            buf[n] = C64_URC_PIN[n];
            n++;
        }
        buf[n] = 0;
        return n;
    }
#endif
    (void)key;
    return 0;                                /* listen/remote-serial: bare  */
}

/* ---- 2. what unoauto_compat.c would have supplied ------------------------ */
/* The kernel-ring sink: unoauto mirrors EVERY channel through uno_dbg_log,
 * so this is where the URC's own narration ("remote: link up", the verb
 * audit lines, LOG frames) lands in the eMMC log -- prefixed so a reader can
 * tell unoauto's lines from this layer's. Except for lines that CAME from
 * this log (the stream below hands them to unoauto to reach the dev PC):
 * those are already here, and mirroring them back would double every line. */
static int g_streaming;

void uno_dbg_log(const char *fmt, ...)
{
    __builtin_va_list ap;
    if (g_streaming)
        return;
    c64_log("ua: ");
    __builtin_va_start(ap, fmt);
    c64_logv(fmt, ap);
    __builtin_va_end(ap);
    c64_log("\n");
}

void uno_dbg_check(const char *tag)
{
    (void)tag;
}

/* A real clock. The URC `uptime` verb, the test runner's wall-clock budget
 * and the screen recorder's frame clock all read this. CNTPCT counts from
 * power-on at CNTFRQ (13 MHz here). */
unsigned long long uno_dbg_uptime_ms(void)
{
    c64_u64 hz = c64_cnt_freq();
    if (!hz)
        hz = 13000000ull;
    return c64_cnt_now() / (hz / 1000ull);
}

/* The dead-man's switch is the debug watchdog ISR on x86; there is none here,
 * and answering "not armed" is what makes guard/pet/safe tell the operator
 * the safety net is absent rather than hand them a fake one. */
void uno_dbg_guard_arm(unsigned timeout_ms) { (void)timeout_ms; }
void uno_dbg_guard_pet(void) { }
void uno_dbg_guard_clear(void) { }
int uno_dbg_guard_armed(void) { return 0; }

/* No draw profiler: the probe's window rows come back without timing. */
int uno_dbg_win_stat(int i, const char **title, unsigned long long *cyc,
                     unsigned long *max_us)
{
    (void)i; (void)title; (void)cyc; (void)max_us;
    return 0;
}
unsigned long uno_dbg_frames(void) { return 0; }
unsigned long uno_dbg_win_calls(int i) { (void)i; return 0; }
unsigned long long uno_dbg_tsc_per_ms(void) { return 0; }

/* CRASH\NETLOG.TXT is a debug-stick artifact; nothing to register. */
void pc64_netlog_sink_ensure(void) { }

/* ---- 3. what this machine cannot have ------------------------------------ */
/* Token minting wants defensible entropy and there is no RDRAND on this core;
 * 0 = "none available", on which the gate refuses to arm rather than mint a
 * weak token. The urc-auth path takes its token from the build instead. */
int tls_entropy_get(unsigned char *out, int n)
{
    (void)out; (void)n;
    return 0;
}

/* UEFI boot-order authoring (`bootnext`, `makeboot`): no firmware variables
 * on an LK payload. 0 = refused, which is how the verbs report it. */
int uno_pc64_set_bootnext(unsigned int n)
{
    (void)n;
    return 0;
}

int uno_pc64_add_boot_entry(void)
{
    return 0;
}

/* ---- 4. the serial transport: PL011 on the QEMU virt board --------------- */
/* Registers per the PrimeCell UART (PL011) TRM. QEMU's model needs no clock
 * or baud programming; the FIFO is enabled so a burst of up to 16 bytes can
 * sit between polls. The transport is polled once per shell frame by
 * unoauto_remote_tick, so inbound throughput is bounded by FIFO x frame
 * rate plus the short top-up below -- plenty for verbs, not for `put`. On
 * the device this is never selected (the cfg reader answers `listen`), and
 * uart_write reports failure rather than pretending. */
#define PL011 0x09000000ull
#define UARTDR (*(volatile c64_u32 *)(PL011 + 0x00))
#define UARTFR (*(volatile c64_u32 *)(PL011 + 0x18))
#define UARTLCR_H (*(volatile c64_u32 *)(PL011 + 0x2C))
#define UARTCR (*(volatile c64_u32 *)(PL011 + 0x30))
#define UARTIMSC (*(volatile c64_u32 *)(PL011 + 0x38))
#define FR_TXFF (1u << 5)
#define FR_RXFE (1u << 4)

static int g_uart_ok;

void uart_init(unsigned base, unsigned baud)
{
    (void)base; (void)baud;                  /* fixed: the virt board's PL011 */
    if (!on_virt()) {
        c64_log("urc: serial transport requested off the QEMU gate -- no UART "
                "on this device, transport stays down\n");
        g_uart_ok = 0;
        return;
    }
    UARTCR = 0;
    UARTIMSC = 0;                            /* polled: no interrupts        */
    UARTLCR_H = (3u << 5) | (1u << 4);       /* 8 bits, FIFOs on, no parity  */
    UARTCR = (1u << 0) | (1u << 8) | (1u << 9);   /* UARTEN | TXE | RXE      */
    __asm__ volatile("dsb sy" ::: "memory");
    g_uart_ok = 1;
    c64_log("urc: serial transport on the PL011 at 0x09000000\n");
}

int uart_write(const char *buf, int n)
{
    if (!g_uart_ok)
        return -1;
    for (int i = 0; i < n; i++) {
        c64_u64 until = c64_cnt_now() + c64_cnt_freq() / 100ull;   /* 10 ms */
        while ((UARTFR & FR_TXFF) && c64_cnt_now() < until)
            ;
        UARTDR = (c64_u8)buf[i];
    }
    return n;
}

int uart_read(unsigned char *buf, int cap)
{
    if (!g_uart_ok)
        return 0;
    int n = 0;
    /* drain what is there, then top up briefly: the host writes a whole line
     * at once and QEMU refills the 16-byte FIFO as fast as it is emptied */
    c64_u64 until = c64_cnt_now() + c64_cnt_freq() / 2000ull;      /* 0.5 ms */
    while (n < cap) {
        if (UARTFR & FR_RXFE) {
            if (!n || c64_cnt_now() >= until)
                break;
            continue;
        }
        buf[n++] = (unsigned char)UARTDR;
    }
    return n;
}

/* ---- the platform log, live, on the dev PC ------------------------------- */
/* Everything this layer says -- msdc:, usb-bulk:, net:, perf:, the drivers'
 * pc64: lines -- goes to the DRAM ring and the eMMC, which until now meant a
 * reboot into Linux to read any of it. unoauto's LOG channels already stream
 * to a connected dev PC (remote_sink in unoauto_remote.c), so the ring is
 * handed to unoauto a line at a time: on every connect, a replay of the
 * recent past (the last STREAM_BACK bytes, so a fresh dial-in gets the boot
 * story), then whatever is logged from then on, as it is logged. Paced at a
 * few lines per frame so the link's 8 KB transmit queue never overflows and
 * a burst of driver chatter cannot stall the shell.
 *
 * The cursor is an ABSOLUTE byte position (c64_log_total counts bytes ever
 * written); the ring's oldest surviving byte is total - size, so a cursor
 * that fell behind a wrap is simply moved up and the gap is reported. */
/* How much history a connect gets. 32 KB was the M6 value and it cost a real
 * debugging session on 2026-09-03: the M7 boot story had scrolled out of the
 * replay window by the time anyone dialled in, so the SD and CoDi bring-up
 * lines -- the whole reason for the boot -- were unreadable while still
 * sitting in the device's own 256 KB ring. The ring is the constraint, not
 * this; take a third of it. The replay is paced at STREAM_LINES a frame, so a
 * bigger window costs seconds of catch-up on connect, not a stalled shell. */
#define STREAM_BACK   (96u * 1024u)
#define STREAM_LINES  8                      /* per frame                       */
static unsigned g_cursor;                    /* absolute byte position          */
static int g_was_active;
static char g_line[256];
static unsigned g_linelen;

void unoauto_log(int ch, const char *fmt, ...);
int unoauto_remote_active(void);

static void stream_log(void)
{
    int active = unoauto_remote_active();
    if (!active) {
        g_was_active = 0;
        return;
    }
    unsigned total = c64_log_total(), size = c64_log_bytes();
    unsigned oldest = total - size;
    if (!g_was_active) {                     /* a new link: start from the past */
        g_was_active = 1;
        g_cursor = total > STREAM_BACK ? total - STREAM_BACK : 0;
        if (g_cursor < oldest)
            g_cursor = oldest;
        g_linelen = 0;
        /* skip to the next line start so the first line is whole */
        c64_u8 c;
        while (g_cursor < total) {
            c64_log_read(g_cursor - oldest, &c, 1);
            g_cursor++;
            if (c == '\n')
                break;
        }
        g_streaming = 1;
        unoauto_log(0, "urc: streaming the platform log from %u bytes back "
                       "(%u logged so far)", total - g_cursor, total);
        g_streaming = 0;
    }
    if (g_cursor < oldest) {                 /* fell behind a ring wrap */
        g_streaming = 1;
        unoauto_log(0, "urc: log stream fell behind, %u bytes skipped",
                    oldest - g_cursor);
        g_streaming = 0;
        g_cursor = oldest;
        g_linelen = 0;
    }
    int lines = 0;
    while (g_cursor < total && lines < STREAM_LINES) {
        c64_u8 buf[128];
        unsigned n = total - g_cursor;
        if (n > sizeof buf)
            n = sizeof buf;
        c64_log_read(g_cursor - oldest, buf, n);
        unsigned i;
        for (i = 0; i < n && lines < STREAM_LINES; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                g_line[g_linelen] = 0;
                if (g_linelen) {
                    g_streaming = 1;
                    unoauto_log(0, "%s", g_line);   /* UA_CH_KERNEL */
                    g_streaming = 0;
                    lines++;
                }
                g_linelen = 0;
            } else if (g_linelen < sizeof g_line - 1) {
                g_line[g_linelen++] = c;
            }
        }
        g_cursor += i;
    }
}

/* ---- bring-up ------------------------------------------------------------ */
void unoauto_remote_boot(void);

void c64_urc_tick(void)
{
    static int booted;
    if (booted) {
        stream_log();
        return;
    }
    if (!c64_net_boot_ran())
        return;
    booted = 1;
    c64_logf("urc: bringing the remote channel up (%s)\n",
             on_virt() ? "serial, QEMU gate" : "listen :5099");
    c64_log_flush();
    unoauto_remote_boot();
}
