/* ===========================================================================
 * UNOAUTOMATE remote channel - see unoauto_remote.h.
 *
 * A cooperative, non-blocking TCP client that dials the dev PC and speaks the
 * URC line protocol: it streams LOG lines out (via an unoauto sink), executes
 * inbound commands against the same DRIVE/PROBE/TEST surface the Python
 * `unoauto` module uses, and hands MSG payloads to Python consumers.
 * ======================================================================== */
#include "unoauto.h"
#include "unoauto_remote.h"
#include "unoauto_gate.h"   /* the privilege gate: who may run which verb */
#include "unosecure.h"      /* enter/leave the link's REMOTE session for `py` */
#include "net.h"            /* u8/u16, net_tcp_*, net_poll */
#include "netsock.h"        /* multi-connection socket API (own link socket) */
#include "netdisc.h"        /* zero-config discovery: auto-dial the found host */
#include "pc64_http.h"      /* pc64_net_up */
#include "iwlwifi.h"        /* iwl_dbg_cmd - the `iwl` verb (F12 live debug) */
#include "unostorage.h"     /* disk authoring (brings blkdev.h): the disk verbs */
#include "unoauto_serial.h" /* 16550 UART backend: the NIC-independent transport */
#include "unoauto_screen.h" /* framebuffer QOI grab: the `screen` verb (remote desktop) */
#include "unolog.h"         /* the system log: what the listener is doing */


/* ---- freestanding libc + debug kernel symbols (no public header) --------- */
void        *memcpy(void *, const void *, unsigned long);
void        *memmove(void *, const void *, unsigned long);
unsigned long strlen(const char *);
void  uno_pc64_inject_key(int scan, int uni, int ctrl);
void  uno_pc64_inject_pointer(int x, int y, int btn);
int   uno_pc64_input_locked(void);   /* a security dialog is modal at the console */
int   pc64_shell_app_count(void);
int   pc64_shell_launch(int a);
int   pc64_shell_app_by_id(const char *id);      /* the registry: id -> slot   */
const char *pc64_shell_app_id(int a);
const char *pc64_shell_app_name(int a);
void  pc64_shell_apps_rescan(void);              /* re-read APPS\ (no reboot)  */
void  pc64_shell_close_top(void);
void  uno_pc64_shutdown(void);
unsigned long long uno_dbg_uptime_ms(void);
/* host-attested guard (uno_debug.c) - the dead-man's switch behind `guard`/
 * `pet`/`safe`. Declared locally (like uptime above) so this file needn't pull
 * in uno_debug.h. arm(0) or clear() disarms; pet() refreshes if armed. */
void  uno_dbg_guard_arm(unsigned timeout_ms);
void  uno_dbg_guard_pet(void);
void  uno_dbg_guard_clear(void);
int   uno_dbg_guard_armed(void);
int   pc64_stress_cfg_value(const char *key, char *buf, int cap);
int   pc64_stress_cfg_flag(const char *key);
int   pc64_shell_py_exec(const char *src, char *out, int cap);   /* pc64_uui.c */
/* A/B OS-update over the link: fs write + reboot (see REMOTE.md) */
int   uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
long  uno_fs_size(int vol, const char *name);
int   uno_fs_writable(int vol);
int   uno_fs_kind(int vol);
int   uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);
void  uno_native_reset(void);
int   uno_pc64_set_bootnext(unsigned int n);                     /* uefi_main.c */
int   uno_pc64_add_boot_entry(const void *disk_dp, unsigned long long first,
                              unsigned long long last, const unsigned char guid[16],
                              const char *desc, const char *path, int make_default);
/* raw-disk authoring (disks/arm/prepdisk verbs) - wraps unostorage + fat + fs */
void  uno_fat_remount(void);                                     /* fat.c   */
void  uno_fat_sync(void);                                        /* fat.c   */
void  uno_fs_remap(void);                                        /* pc64_fs */
int   uno_fs_mkdir(int vol, const char *path);                   /* pc64_fs */
int   uno_fs_isdir(int vol, const char *path);                   /* pc64_fs */
void *uno_fs_vol_bdev(int vol);                                  /* pc64_fs */
int   uno_fs_copytree(int src_vol, int dst_vol,                  /* pc64_fs */
                      unsigned char *scratch, long cap, long *out_bytes);
int   uno_fat_mkfs(uno_bdev *dev, unsigned long long first,
                   unsigned long long sectors, const char *label);
int   uno_fat_mkdir(int fatvol, const char *path);               /* fat.c   */
int   uno_fs_fat_index(int vol);                                 /* pc64_fs */

/* ---- tiny string builder (avoids snprintf; see the S-LIBC-06 history) ---- */
typedef struct { char *p; int cap, len; } SB;
static void sb_init(SB *b, char *buf, int cap) { b->p = buf; b->cap = cap; b->len = 0; }
static void sb_c(SB *b, char c)   { if (b->len < b->cap - 1) b->p[b->len++] = c; }
static void sb_s(SB *b, const char *s) { while (*s) sb_c(b, *s++); }
static void sb_i(SB *b, long v)
{
    char t[24]; int n = 0; unsigned long u;
    if (v < 0) { sb_c(b, '-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) { sb_c(b, '0'); return; }
    while (u) { t[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) sb_c(b, t[--n]);
}
static void sb_ull(SB *b, unsigned long long v)   /* 64-bit (LBAs/sector counts) */
{
    char t[24]; int n = 0;
    if (!v) { sb_c(b, '0'); return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) sb_c(b, t[--n]);
}

/* ---- link state ---------------------------------------------------------- */
enum { RS_OFF = 0, RS_DISCOVER, RS_CONNECTING, RS_UP, RS_DOWN };
static int      g_state;
static int      g_sock = -1;        /* our OWN socket - no longer the shared
                                       legacy net_tcp_* slot, so the Browser/AI
                                       apps can hold a connection alongside us */
static u8       g_ip[4];
static u16      g_port;
static int      g_sink = -1;
static unsigned g_tick;
static unsigned g_deadline;         /* connect timeout / retry-at (in ticks)  */
static int      g_pending_off;      /* shut down once the TX queue drains      */
static unsigned g_uart_base;        /* serial transport: 16550 I/O base (or 0) */
static unsigned g_hello_at;         /* serial: re-emit HELLO at this tick ...   */
static int      g_rx_seen;          /* ... until the host is heard from once    */
static int      g_listening;        /* URC SERVER mode (`listen`): host dials IN */
static int      g_listen_sock = -1; /* the bound+listening socket (listen mode)  */

/* outbound byte queue (linear, compacted on flush) */
static char     g_tx[8192];
static int      g_txlen;
static unsigned g_tx_dropped;

/* inbound line assembly. 4 KB so a `put` frame can carry a big base64 chunk
 * (fewer synchronous round-trips = faster multi-MB pushes); still well inside
 * the 8 KB TCP rxq. */
static char     g_rx[4096];
static int      g_rxlen;

/* Auth attempts are capped per drain_rx pass (item 1c): drain_rx dispatches
 * every newline-delimited line in one socket read, so without a cap an attacker
 * could pipeline hundreds of `auth` guesses into a single window.  The gate's
 * three-strike lockout (BADAUTH_MAX) already stands the channel down, but this
 * is a cheap belt-and-braces bound so the pipeline is throttled at the source
 * even before the gate reacts. */
#define AUTH_PER_DRAIN_MAX 4
static int      g_auth_in_drain;

/* inbound MSG queue handed to Python via unoauto_remote_recv */
#define INQN 8
#define INQL 256
static char     g_inq[INQN][INQL];
static int      g_in_head, g_in_tail;

/* ---- outbound framing ---------------------------------------------------- */
static void tx_putn(const char *s, int n)
{
    if (n <= 0) return;
    if (g_txlen + n > (int)sizeof g_tx) { g_tx_dropped += (unsigned)n; return; }
    memcpy(g_tx + g_txlen, s, (unsigned long)n);
    g_txlen += n;
}

static const char *chan_name(UnoAutoChan ch)
{
    switch (ch) {
    case UA_CH_KERNEL:  return "KERNEL";
    case UA_CH_NET:     return "NET";
    case UA_CH_UI:      return "UI";
    case UA_CH_STORAGE: return "STORAGE";
    case UA_CH_TEST:    return "TEST";
    case UA_CH_SCRIPT:  return "SCRIPT";
    default:            return "?";
    }
}

/* the LOG spine: every channel line becomes a `LOG <chan> <text>` frame while
 * the link is up.  Registered as an unoauto sink so producers never know. */
static void remote_sink(UnoAutoChan ch, const char *line, void *user)
{
    char f[600]; SB b; (void)user;
    if (g_state != RS_UP) return;
    sb_init(&b, f, sizeof f);
    sb_s(&b, "LOG "); sb_s(&b, chan_name(ch)); sb_c(&b, ' '); sb_s(&b, line);
    sb_c(&b, '\n');
    tx_putn(f, b.len);
}

/* one `RSP <id> <status> [text]` frame (id echoed verbatim). */
static void rsp(const char *id, const char *status, const char *text)
{
    char f[600]; SB b;
    sb_init(&b, f, sizeof f);
    sb_s(&b, "RSP "); sb_s(&b, id); sb_c(&b, ' '); sb_s(&b, status);
    if (text && *text) { sb_c(&b, ' '); sb_s(&b, text); }
    sb_c(&b, '\n');
    tx_putn(f, b.len);
}

int unoauto_remote_send(const char *type, const char *text)
{
    char f[600]; SB b; int before = g_txlen;
    if (g_state != RS_UP) return -1;
    sb_init(&b, f, sizeof f);
    sb_s(&b, type ? type : "MSG");
    if (text && *text) { sb_c(&b, ' '); sb_s(&b, text); }
    sb_c(&b, '\n');
    tx_putn(f, b.len);
    return g_txlen > before ? b.len : -1;
}

/* ---- inbound MSG queue --------------------------------------------------- */
static void inq_push(const char *s)
{
    int i = 0;
    int nxt = (g_in_head + 1) % INQN;
    if (nxt == g_in_tail) g_in_tail = (g_in_tail + 1) % INQN;  /* drop oldest */
    while (s[i] && i < INQL - 1) { g_inq[g_in_head][i] = s[i]; i++; }
    g_inq[g_in_head][i] = 0;
    g_in_head = nxt;
}

int unoauto_remote_recv(char *buf, int cap)
{
    int i = 0; const char *s;
    if (g_in_tail == g_in_head || cap <= 0) { if (cap > 0) buf[0] = 0; return 0; }
    s = g_inq[g_in_tail];
    while (s[i] && i < cap - 1) { buf[i] = s[i]; i++; }
    buf[i] = 0;
    g_in_tail = (g_in_tail + 1) % INQN;
    return i;
}

/* ---- token helpers ------------------------------------------------------- */
/* next whitespace-delimited token: NUL-terminates it in place, advances *s. */
static char *tok(char **s)
{
    char *p = *s, *start;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *s = p; return 0; }
    start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *p = 0; p++; }
    *s = p;
    return start;
}
static void skip_ws(char **s) { while (**s == ' ' || **s == '\t') (*s)++; }
static long atol_(const char *s)
{
    long v = 0; int neg = 0;
    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
/* 0 when equal (like strcmp==0), tolerant of NULL a. */
static int strcmp_(const char *a, const char *b)
{
    if (!a) return 1;
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static unsigned long parse_hex(const char *s)
{
    unsigned long v = 0;
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = (v << 4) | (unsigned long)d;
    }
    return v;
}
static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
/* decode NUL-terminated base64 into out (cap bytes); bytes decoded, or -1.
 * acc is UNSIGNED: the reservoir keeps shifting left, and a signed overflow
 * there is UB (traps under UBSan as #UD - it bit the first long `put`). */
static int b64_decode(const char *s, unsigned char *out, int cap)
{
    unsigned acc = 0; int nbits = 0, n = 0;
    for (; *s; s++) {
        int v;
        if (*s == '=') break;
        v = b64val(*s);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v; nbits += 6;
        if (nbits >= 8) { nbits -= 8; if (n >= cap) return -1;
                          out[n++] = (unsigned char)((acc >> nbits) & 0xFF); }
    }
    return n;
}
static unsigned long long parse_hex64(const char *s)
{
    unsigned long long v = 0;
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = (v << 4) | (unsigned)d;
    }
    return v;
}
/* base64-encode `n` bytes into out (NUL-terminated); returns length, or -1. */
static int b64_encode(const unsigned char *in, int n, char *out, int cap)
{
    static const char B[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, o = 0;
    while (i < n) {
        unsigned v = (unsigned)in[i] << 16; int have = 1;
        if (i + 1 < n) { v |= (unsigned)in[i + 1] << 8; have = 2; }
        if (i + 2 < n) { v |= in[i + 2]; have = 3; }
        if (o + 4 > cap - 1) return -1;
        out[o++] = B[(v >> 18) & 63];
        out[o++] = B[(v >> 12) & 63];
        out[o++] = have >= 2 ? B[(v >> 6) & 63] : '=';
        out[o++] = have >= 3 ? B[v & 63] : '=';
        i += 3;
    }
    out[o] = 0;
    return o;
}

/* ---- command dispatch ---------------------------------------------------- */
/* 96, not 64: the probe now also emits the per-window draw profile (up to
 * WPROF_N=24 rows), and put() drops silently past the end - which would have
 * truncated the module roster instead of reporting anything. ~40 bytes a row,
 * so this is 1.3 KB of .bss, not the 24 MiB a PUT_MAX rise once cost. */
static UnoAutoProbeEnt g_pe[96];
static char            g_report[4096];

static void do_probe(const char *id)
{
    int n = unoauto_probe(g_pe, 96), i;
    for (i = 0; i < n; i++) {
        /* fields: kind state v1 v2 name - name LAST so it may contain spaces */
        char f[256]; SB b;
        sb_init(&b, f, sizeof f);
        sb_i(&b, g_pe[i].kind);  sb_c(&b, ' ');
        sb_i(&b, g_pe[i].state); sb_c(&b, ' ');
        /* sb_ull, NOT sb_i: v1/v2 are u64 and sb_i takes a `long`, which is
         * 32 BITS on this PE target.  The window rows carry accumulated draw
         * TSC, which passes 2^32 after about two seconds of drawing at 2 GHz,
         * so every cycle total reported here used to wrap - silently, and into
         * a NEGATIVE number once the cast went through signed long. */
        sb_ull(&b, g_pe[i].v1); sb_c(&b, ' ');
        sb_ull(&b, g_pe[i].v2); sb_c(&b, ' ');
        sb_s(&b, g_pe[i].name ? g_pe[i].name : "?");
        f[b.len] = 0;
        rsp(id, "ok", f);
    }
    rsp(id, "end", 0);
}

static void do_test(const char *id, char *suite)
{
    char *p = g_report; int rc;
    if (suite && !*suite) suite = 0;
    rc = unoauto_test_run(suite, 0, g_report, (int)sizeof g_report);
    /* stream the report line by line */
    while (*p) {
        char *nl = p;
        while (*nl && *nl != '\n') nl++;
        { char save = *nl; *nl = 0; rsp(id, "ok", p); *nl = save; }
        if (!*nl) break;
        p = nl + 1;
    }
    { char t[32]; SB b; sb_init(&b, t, sizeof t); sb_s(&b, "rc="); sb_i(&b, rc); t[b.len] = 0;
      rsp(id, rc == 0 ? "ok" : "err", t); }
    rsp(id, "end", 0);
}

/* ---- A/B OS-update: chunked file push + reboot ---------------------------
 * `put` RAM-stages the whole file (base64 chunks at rising offsets), then a
 * `put <vol> <path> done <total>` frame writes it to disk in ONE uno_fs_write
 * and verifies the on-disk size - so a partial transfer never touches the
 * target (stick A stays a valid fallback).  8 MB cap covers BOOTX64.EFI. */
#define PUT_MAX (8 * 1024 * 1024)
static unsigned char g_put[PUT_MAX];      /* debug-only staging buffer (.bss) */
static long          g_put_len;
static int           g_put_vol = -1;
static char          g_put_path[80];
static int           g_pending_reboot;

static void do_put(const char *id, char *args)
{
    char *a_vol  = tok(&args);
    char *a_path = tok(&args);
    char *a3     = tok(&args);
    char *a4     = tok(&args);
    int vol;
    char t[64]; SB b;
    if (!a_vol || !a_path || !a3) {
        rsp(id, "err", "usage: put <vol> <path> <off-hex|done> <chunk|total>");
        rsp(id, "end", 0); return;
    }
    vol = (int)atol_(a_vol);

    if (!strcmp_(a3, "done")) {                        /* finalize + verify */
        long total = (long)parse_hex(a4);
        long sz;
        if (g_put_vol != vol || strcmp_(g_put_path, a_path) != 0) {
            rsp(id, "err", "no-active-upload"); rsp(id, "end", 0); return;
        }
        if (g_put_len != total) {
            sb_init(&b, t, sizeof t); sb_s(&b, "size-mismatch have="); sb_i(&b, g_put_len);
            sb_s(&b, " want="); sb_i(&b, total); t[b.len] = 0;
            rsp(id, "err", t); rsp(id, "end", 0); return;
        }
        if (!uno_fs_write(vol, g_put_path, g_put, g_put_len)) {
            rsp(id, "err", "write-failed (vol read-only or full?)");
            rsp(id, "end", 0); g_put_vol = -1; return;
        }
        sz = uno_fs_size(vol, g_put_path);
        if (sz != total) {
            sb_init(&b, t, sizeof t); sb_s(&b, "verify-mismatch disk="); sb_i(&b, sz); t[b.len] = 0;
            rsp(id, "err", t); rsp(id, "end", 0); g_put_vol = -1; return;
        }
        sb_init(&b, t, sizeof t); sb_s(&b, "verified "); sb_i(&b, total); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0);
        g_put_vol = -1;                                /* session complete */
        return;
    }

    {                                                  /* data chunk */
        long off = (long)parse_hex(a3);
        int n;
        if (!a4) { rsp(id, "err", "missing-chunk"); rsp(id, "end", 0); return; }
        if (off < 0 || off >= PUT_MAX) { rsp(id, "err", "offset-too-big"); rsp(id, "end", 0); return; }
        if (off == 0) {                                /* (re)start a session */
            int i = 0;
            g_put_vol = vol; g_put_len = 0;
            while (a_path[i] && i < (int)sizeof g_put_path - 1) { g_put_path[i] = a_path[i]; i++; }
            g_put_path[i] = 0;
        } else if (g_put_vol != vol || strcmp_(g_put_path, a_path) != 0) {
            rsp(id, "err", "out-of-sequence (start at offset 0)"); rsp(id, "end", 0); return;
        }
        n = b64_decode(a4, g_put + off, (int)(PUT_MAX - off));
        if (n < 0) { rsp(id, "err", "bad-base64-or-too-big"); rsp(id, "end", 0); return; }
        if (off + n > g_put_len) g_put_len = off + n;
        sb_init(&b, t, sizeof t); sb_i(&b, n); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0);
    }
}

/* list volumes so the host knows which index is stick B: `vol kind writable name`
 * (kind 0=RAM 1=native-FAT 2=firmware-SFS). */
static void do_vols(const char *id)
{
    int n = uno_fs_volumes(), i;
    for (i = 0; i < n; i++) {
        char f[128]; SB b;
        sb_init(&b, f, sizeof f);
        sb_i(&b, i);                 sb_c(&b, ' ');
        sb_i(&b, uno_fs_kind(i));    sb_c(&b, ' ');
        sb_i(&b, uno_fs_writable(i)); sb_c(&b, ' ');
        sb_s(&b, uno_fs_volume_name(i));
        f[b.len] = 0;
        rsp(id, "ok", f);
    }
    rsp(id, "end", 0);
}

/* ---- raw-disk authoring (partition/format disk B) ------------------------
 * Thin wrappers over unostorage + fat + pc64_fs - no storage logic here.
 * Destructive verbs require an explicit `arm <disk>` that AUTO-DISARMS after
 * one op; `arm` refuses the boot disk.  See REMOTE.md. */
static int g_armed_disk = -1;

/* emit a long string as multiple RSP `ok` lines (rsp's buffer is small); the
 * host concatenates them.  Used for readsec's base64. */
static void rsp_long(const char *id, const char *s)
{
    int len = (int)strlen(s), off = 0;
    while (off < len) {
        char c[500]; int k = 0;
        while (off < len && k < 480) c[k++] = s[off++];
        c[k] = 0;
        rsp(id, "ok", c);
    }
}

static uno_bdev *disk_at(int i)
{ return (i >= 0 && i < uno_blk_count()) ? uno_blk_get(i) : 0; }

/* validate the arm gate for a destructive op; consumes the arm (auto-disarm). */
static uno_bdev *armed_bdev(const char *id, int disk)
{
    uno_bdev *b;
    if (g_armed_disk != disk) {
        rsp(id, "err", "not-armed (arm <disk> first)"); rsp(id, "end", 0); return 0;
    }
    g_armed_disk = -1;                                  /* auto-disarm */
    b = disk_at(disk);
    if (!b || !b->write) { rsp(id, "err", "bad-disk or read-only"); rsp(id, "end", 0); return 0; }
    return b;
}

static void do_disks(const char *id)
{
    int n = uno_blk_count(), i;
    for (i = 0; i < n; i++) {
        uno_bdev *b = uno_blk_get(i);
        char f[96]; SB sb;
        if (!b) continue;
        sb_init(&sb, f, sizeof f);
        sb_i(&sb, i);                sb_c(&sb, ' ');    /* idx name sectors writable is_boot */
        sb_s(&sb, b->name);          sb_c(&sb, ' ');
        sb_ull(&sb, b->sectors);     sb_c(&sb, ' ');
        sb_i(&sb, b->write ? 1 : 0); sb_c(&sb, ' ');
        sb_i(&sb, b->is_boot);
        f[sb.len] = 0;
        rsp(id, "ok", f);
    }
    rsp(id, "end", 0);
}

static void do_arm(const char *id, int disk)
{
    uno_bdev *b = disk_at(disk);
    char f[64]; SB sb;
    if (!b)          { rsp(id, "err", "bad-disk");                     rsp(id, "end", 0); return; }
    if (b->is_boot)  { rsp(id, "err", "refused: that is the boot disk"); rsp(id, "end", 0); return; }
    if (!b->write)   { rsp(id, "err", "disk is read-only");            rsp(id, "end", 0); return; }
    g_armed_disk = disk;
    sb_init(&sb, f, sizeof f);
    sb_s(&sb, "armed "); sb_s(&sb, b->name); sb_c(&sb, ' '); sb_ull(&sb, b->sectors); sb_s(&sb, " sectors");
    f[sb.len] = 0;
    rsp(id, "ok", f); rsp(id, "end", 0);
}

static void do_readsec(const char *id, char *args)      /* non-destructive */
{
    static unsigned char sec[4 * 512];
    static char b64[4 * 512 * 2];
    int disk = (int)atol_(tok(&args));
    unsigned long long lba = parse_hex64(tok(&args));
    char *ns = tok(&args);
    int n = ns ? (int)atol_(ns) : 1, enc;
    uno_bdev *b = disk_at(disk);
    if (!b || !b->read) { rsp(id, "err", "bad-disk"); rsp(id, "end", 0); return; }
    if (n < 1) n = 1; if (n > 4) n = 4;
    if (!b->read(b, lba, (unsigned)n, sec)) { rsp(id, "err", "read failed"); rsp(id, "end", 0); return; }
    enc = b64_encode(sec, n * 512, b64, (int)sizeof b64);
    if (enc < 0) { rsp(id, "err", "encode"); rsp(id, "end", 0); return; }
    rsp_long(id, b64);
    rsp(id, "end", 0);
}

static void do_writesec(const char *id, char *args)     /* destructive */
{
    static unsigned char sec[4 * 512];
    int disk = (int)atol_(tok(&args));
    unsigned long long lba = parse_hex64(tok(&args));
    char *b64 = tok(&args);
    int n, secs;
    uno_bdev *b = armed_bdev(id, disk);
    if (!b) return;
    if (!b64) { rsp(id, "err", "missing-data"); rsp(id, "end", 0); return; }
    n = b64_decode(b64, sec, (int)sizeof sec);
    if (n <= 0 || (n % 512)) { rsp(id, "err", "data must be whole 512B sectors"); rsp(id, "end", 0); return; }
    secs = n / 512;
    if (!b->write(b, lba, (unsigned)secs, sec)) { rsp(id, "err", "write failed"); rsp(id, "end", 0); return; }
    { char t[16]; SB sb; sb_init(&sb, t, sizeof t); sb_i(&sb, secs); t[sb.len] = 0;
      rsp(id, "ok", t); }
    rsp(id, "end", 0);
}

static void do_gptinit(const char *id, char *args)      /* destructive */
{
    int disk = (int)atol_(tok(&args));
    uno_bdev *b = armed_bdev(id, disk);
    unostorage_dev d; int ok;
    if (!b) return;
    d = unostorage_from_bdev(b);
    ok = unostorage_gpt_init(&d);
    rsp(id, ok ? "ok" : "err", ok ? "gpt" : "failed");
    rsp(id, "end", 0);
}

static void do_mkpart(const char *id, char *args)       /* destructive */
{
    int disk = (int)atol_(tok(&args));
    unsigned long long first = parse_hex64(tok(&args));
    unsigned long long last  = parse_hex64(tok(&args));
    char *type = tok(&args), *name = tok(&args);
    uno_bdev *b = armed_bdev(id, disk);
    unostorage_dev d;
    if (!b) return;
    if (!type || strcmp_(type, "esp")) { rsp(id, "err", "type must be 'esp'"); rsp(id, "end", 0); return; }
    d = unostorage_from_bdev(b);
    rsp(id, unostorage_gpt_add(&d, first, last, unostorage_esp_type, name ? name : "UNO-ESP")
        ? "ok" : "err", "part");
    rsp(id, "end", 0);
}

static void do_mkfs(const char *id, char *args)         /* destructive */
{
    int disk = (int)atol_(tok(&args));
    unsigned long long first = parse_hex64(tok(&args));
    unsigned long long secs  = parse_hex64(tok(&args));
    char *label = tok(&args);
    uno_bdev *b = armed_bdev(id, disk);
    if (!b) return;
    if (uno_fat_mkfs(b, first, secs, label ? label : "UNODOS")) {
        uno_fat_remount(); uno_fs_remap();
        rsp(id, "ok", "formatted");
    } else rsp(id, "err", "mkfs failed (too small / read-only?)");
    rsp(id, "end", 0);
}

static void do_prepdisk(const char *id, char *args)     /* destructive (GPT+ESP+format) */
{
    int disk = (int)atol_(tok(&args));
    char *label = tok(&args);
    uno_bdev *b = armed_bdev(id, disk);
    if (!b) return;
    if (unostorage_prepare_esp(b, label ? label : "UNODOS")) {
        uno_fat_remount(); uno_fs_remap();
        rsp(id, "ok", "prepared");
    } else rsp(id, "err", "prepare failed (too small / read-only?)");
    rsp(id, "end", 0);
}

/* makeboot <disk> [desc] [efi-path]: author a UEFI boot entry for the ESP on
 * <disk> (after prepdisk + pushing the OS files onto it) so the machine can boot
 * that internal disk.  Not disk-destructive (an NVRAM boot var), so no arm gate;
 * needs firmware runtime services (attached).  Defaults: desc "UnoDOS", path
 * \EFI\BOOT\BOOTX64.EFI, made the default boot entry. */
static void do_makeboot(const char *id, char *args)
{
    int disk = (int)atol_(tok(&args));
    char *desc = tok(&args);
    char *path = tok(&args);
    uno_bdev *b = disk_at(disk);
    unostorage_dev d;
    unsigned long long first, last; unsigned char guid[16];
    if (!b)     { rsp(id, "err", "bad-disk"); rsp(id, "end", 0); return; }
    if (!b->dp) { rsp(id, "err", "no device path (attached firmware disk required)");
                  rsp(id, "end", 0); return; }
    d = unostorage_from_bdev(b);
    if (!unostorage_find_esp(&d, &first, &last, guid)) {
        rsp(id, "err", "no ESP on this disk (prepdisk first)"); rsp(id, "end", 0); return;
    }
    if (uno_pc64_add_boot_entry(b->dp, first, last, guid,
                                desc ? desc : "UnoDOS",
                                path ? path : "\\EFI\\BOOT\\BOOTX64.EFI", 1))
        rsp(id, "ok", "boot-entry added");
    else
        rsp(id, "err", "SetVariable failed (detached / no runtime services?)");
    rsp(id, "end", 0);
}

/* mkdir <vol> <path> - create ONE directory on a mounted volume (like `put`,
 * this is a volume-level op, no `arm` gate).  The parent must already exist, so
 * a nested loader tree is laid down a level at a time (mkdir \EFI ; mkdir
 * \EFI\BOOT ; put \EFI\BOOT\BOOTX64.EFI ...) - the missing primitive that let
 * `put` push flat files but never create the \EFI\BOOT\ a bootable stick needs.
 * Idempotent: an already-existing dir reports `ok exists`, not an error. */
static void do_mkdir(const char *id, char *args)
{
    int vol = (int)atol_(tok(&args));
    char *path = tok(&args);
    if (!path || !path[0]) { rsp(id, "err", "usage: mkdir <vol> <path>"); rsp(id, "end", 0); return; }
    if (!uno_fs_writable(vol)) { rsp(id, "err", "vol not writable"); rsp(id, "end", 0); return; }
    if (uno_fs_mkdir(vol, path)) {
        uno_fat_sync();                              /* persist the dir entry now */
        unoauto_log(UA_CH_SCRIPT, "mkdir vol=%d %s -> created", vol, path);
        rsp(id, "ok", "created"); rsp(id, "end", 0); return;
    }
    /* mkdir failed: idempotent-OK if the path already is a directory, otherwise
     * a real error (parent missing / read-only / disk full). */
    if (uno_fs_isdir(vol, path)) { rsp(id, "ok", "exists"); rsp(id, "end", 0); return; }
    rsp(id, "err", "mkdir failed (parent missing / read-only / full)");
    rsp(id, "end", 0);
}

/* install <disk> [default] - clone the running OS onto disk <disk> over URC,
 * armed like the other destructive verbs (arm echoes size + refuses the boot
 * disk).  Preps a fresh GPT+ESP+FAT32 on the target, then clones the boot ESP's
 * whole tree onto it natively (prepdisk + copytree, built on the mkdir
 * primitive), so the disk boots via the firmware removable-media path
 * \EFI\BOOT\BOOTX64.EFI.  It does NOT write an NVRAM Boot#### entry: runtime
 * SetVariable is refused post-detach (see uno_pc64_set_bootnext) and URC is
 * always post-detach - so `default` is accepted but inert.  A USB stick
 * auto-boots from the removable path; an internal disk boots via firmware
 * fallback or a one-time boot-menu pick.  See REMOTE.md. */
static void do_install(const char *id, char *args)
{
    int disk = (int)atol_(tok(&args));
    char *opt = tok(&args);        /* optional "default" - inert over URC (no NVRAM) */
    uno_bdev *b = armed_bdev(id, disk);   /* arm gate + auto-disarm */
    int nvol, v, src = -1, dst = -1, files;
    long bytes = 0;
    char t[80]; SB sb;
    (void)opt;
    if (!b) return;

    /* 1) fresh GPT + ESP + FAT32 on the target, then remount so it mounts. */
    if (!unostorage_prepare_esp(b, "UNODOS")) {
        rsp(id, "err", "prepare failed (too small / read-only?)"); rsp(id, "end", 0); return;
    }
    uno_fat_remount(); uno_fs_remap();
    rsp(id, "ok", "prepared");

    /* 2) identify source (the boot ESP, by its loader) and target (by device). */
    nvol = uno_fs_volumes();
    for (v = 0; v < nvol; v++) {
        if ((void *)b == uno_fs_vol_bdev(v)) dst = v;
        else if (src < 0 && uno_fs_size(v, "\\EFI\\BOOT\\BOOTX64.EFI") >= 0) src = v;
    }
    if (dst < 0) { rsp(id, "err", "target volume not found after prepare"); rsp(id, "end", 0); return; }
    if (src < 0) { rsp(id, "err", "source ESP not found (no \\EFI\\BOOT\\BOOTX64.EFI)"); rsp(id, "end", 0); return; }
    if (src == dst) { rsp(id, "err", "source == target"); rsp(id, "end", 0); return; }
    rsp(id, "ok", "cloning");

    /* 3) clone the whole boot tree onto the target (reuse the put staging buf). */
    g_put_vol = -1;                          /* invalidate any prior put session */
    files = uno_fs_copytree(src, dst, g_put, PUT_MAX, &bytes);
    if (files < 0) {
        sb_init(&sb, t, sizeof t); sb_s(&sb, "clone failed rc="); sb_i(&sb, files);
        t[sb.len] = 0; rsp(id, "err", t); rsp(id, "end", 0); return;
    }
    uno_fat_sync();                          /* persist before we report done */

    sb_init(&sb, t, sizeof t);
    sb_s(&sb, "installed "); sb_i(&sb, files); sb_s(&sb, " files ");
    sb_ull(&sb, (unsigned long long)bytes);
    sb_s(&sb, " bytes (removable-path boot; no NVRAM entry)");
    t[sb.len] = 0; rsp(id, "ok", t);
    unoauto_log(UA_CH_SCRIPT, "install disk=%d -> %d files %ld bytes", disk, files, bytes);
    rsp(id, "end", 0);
}

/* eth verb pass-through target. The wired-NIC driver (Realtek r8169) lands the
 * real r8169_dbg_cmd() in r8169.c / r8169.h per the 2026-07-22 request in
 * UNOAUTOMATE-REQUESTS.md. We declare it locally (not via r8169.h) so this file
 * builds independently of when the driver hook arrives, and ship a weak fallback
 * so the tree links green in the meantime; once the strong definition lands the
 * linker prefers it - no coordination, no broken intermediate state. Same shape
 * as iwl_dbg_cmd: returns reply length, or -1 (unknown subcmd / NIC not mapped). */
int r8169_dbg_cmd(const char *line, char *out, int cap);
__attribute__((weak)) int r8169_dbg_cmd(const char *line, char *out, int cap)
{
    static const char msg[] = "r8169 debug not built (driver hook pending)";
    int i = 0;
    (void)line;
    if (out && cap > 0) {
        for (; msg[i] && i < cap - 1; i++) out[i] = msg[i];
        out[i] = 0;
    }
    return -1;
}

/* devices verb pass-through target. unodevices (branch `unodevices`, phase 1 of
 * docs/UNODEVICES-PLAN.md) lands the real devmgr_list_str() in uno_devmgr.c/.h
 * per the 2026-07-23 request in UNOAUTOMATE-REQUESTS.md. Declared locally rather
 * than via uno_devmgr.h - same rationale as r8169_dbg_cmd above: this file builds
 * and links green before the provider exists, and the linker prefers the strong
 * definition the moment it arrives. Writes a NUL-terminated multi-line listing
 * into buf and returns its length (excluding the NUL), truncated to cap. */
int devmgr_list_str(char *buf, int cap);
__attribute__((weak)) int devmgr_list_str(char *buf, int cap)
{
    static const char msg[] = "device manager not built (unodevices pending)";
    int i = 0;
    if (buf && cap > 0) {
        for (; msg[i] && i < cap - 1; i++) buf[i] = msg[i];
        buf[i] = 0;
    }
    return -1;
}

/* hwwdt verb pass-through target. unodevices lands the real uno_hw_wdt_cmd() in
 * uno_hw_wdt.c (the PCH TCO hardware watchdog); declared locally + weak-stubbed
 * here, same pattern as above, so this file links green with or without the
 * module. status/arm/pet/disarm/selftest/wedge; see HWWATCHDOG.md. */
int uno_hw_wdt_cmd(const char *line, char *out, int cap);
__attribute__((weak)) int uno_hw_wdt_cmd(const char *line, char *out, int cap)
{
    static const char msg[] = "hw watchdog not built (uno_hw_wdt pending)";
    int i = 0;
    (void)line;
    if (out && cap > 0) {
        for (; msg[i] && i < cap - 1; i++) out[i] = msg[i];
        out[i] = 0;
    }
    return -1;
}

/* stream verb pass-through target. unostream (unostream.c, pc64/UNOSTREAM.md)
 * lands the real unostream_cmd() - outbound TCP screen streaming for demo
 * capture, paced on its own tick and socket (it deliberately does NOT ride
 * this file's 512 B/tick TX pump). Declared locally + weak-stubbed here, the
 * r8169_dbg_cmd pattern above: definition and caller share THIS TU (a weak def
 * in another TU is an undefined reference with this toolchain), and the
 * linker prefers unostream.o's strong definition the moment it is in the
 * link. Per the 2026-08-07 unostream entry in UNOAUTOMATE-REQUESTS.md. */
int unostream_cmd(char *line, char *out, int cap);
__attribute__((weak)) int unostream_cmd(char *line, char *out, int cap)
{
    static const char msg[] = "unostream not built";
    int i = 0;
    (void)line;
    if (out && cap > 0) {
        for (; msg[i] && i < cap - 1; i++) out[i] = msg[i];
        out[i] = 0;
    }
    return -1;
}

/* skin verb pass-through target. UnoAmp (unoamp_app.c) lands the real
 * unoamp_skin_cmd() - re-skin a RUNNING player and repaint it, so a skin
 * change is demonstrable without a reboot. Same shape as the two above:
 * declared locally and weak-stubbed HERE, in this TU, because a weak
 * definition in another TU is an undefined reference with this toolchain, and
 * the linker takes unoamp_app.o's strong definition the moment it is in the
 * link. Per the 2026-08-07 unoamp/skin claim in UNOAUTOMATE-REQUESTS.md. */
int unoamp_skin_cmd(char *line, char *out, int cap);
__attribute__((weak)) int unoamp_skin_cmd(char *line, char *out, int cap)
{
    static const char msg[] = "UnoAmp not built";
    int i = 0;
    (void)line;
    if (out && cap > 0) {
        for (; msg[i] && i < cap - 1; i++) out[i] = msg[i];
        out[i] = 0;
    }
    return -1;
}

/* ssh verb pass-through target. unossh (unossh_cmd.c) lands the real
 * ssh_dbg_cmd() - log into another machine, run a command, hand back its
 * output in bounded slices. Same shape and same reason as the three above:
 * declared locally and weak-stubbed HERE, so this file links with or without
 * unossh in the build. Answers the 2026-08-01 unossh request ("one weak stub
 * and one clause, when convenient"), which the 2026-08-08 demo-lane index
 * found had never been landed: the verb was complete and unreachable, and
 * with no other way to add a key or a session, the SSH client could not be
 * used at all as shipped. */
int ssh_dbg_cmd(const char *line, char *out, int cap);
__attribute__((weak)) int ssh_dbg_cmd(const char *line, char *out, int cap)
{
    static const char msg[] = "unossh not built";
    int i = 0;
    (void)line;
    if (out && cap > 0) {
        for (; msg[i] && i < cap - 1; i++) out[i] = msg[i];
        out[i] = 0;
    }
    return -1;
}

/* session token echoed at `guard` arm; `safe` must present it (a stale disarm
 * from a prior session must not stand a fresh guard down). Cheap, not secret. */
static unsigned g_guard_token;
static int      g_guard_token_minted;   /* a token has been issued -> `safe` needs it */

/* execute `verb args...` (id echoed on every RSP). args is the remainder. */
/* ---- screen grab (remote desktop, OUT half) ------------------------------ */
/* A frame is STAGED on-device by `screen grab`, then the client pulls it in
 * bounded `screen read <off> <len>` slices - the readsec idiom. This is
 * essential: dispatch_cmd runs to completion appending the whole reply to the
 * 8 KB g_tx before flush_tx drains it, and tx_putn silently drops past 8 KB, so
 * a whole frame's base64 (well over 8 KB) can NOT be streamed in one response.
 * Each `read` slice stays comfortably inside g_tx. */
#define SCREEN_MAX (2 * 1024 * 1024)   /* QOI scratch; flat desktop fits at native res */
#define SCREEN_READ_MAX 2880           /* per-`read` payload cap (8*360): base64 fits g_tx */
static unsigned char g_screen[SCREEN_MAX];   /* separate from g_put[] so a grab can't
                                                clobber an in-flight A/B upload */
static int g_screen_len;               /* staged frame size, 0 = none staged */

/* Stream binary `data` as base64 in `ok` lines, 360 raw bytes -> 480 chars each
 * (the same 480-char budget rsp_long uses). Each 360-byte chunk is a multiple
 * of 3, so its base64 has no interior '=' padding and the client can just
 * concatenate the lines before decoding. Mirrors do_readsec's b64 spill. */
static void rsp_b64_stream(const char *id, const unsigned char *data, int n)
{
    char line[512]; int off = 0;
    while (off < n) {
        int chunk = n - off;
        if (chunk > 360) chunk = 360;
        if (b64_encode(data + off, chunk, line, (int)sizeof line) < 0) return;
        rsp(id, "ok", line);
        off += chunk;
    }
}

/* one `ok frames <n> bytes <b> dropped <d> ew <ew> eh <eh> cols <c> tw <t> th <t>
 * fps <f> on <0/1>` line - the constant geometry a client needs to reconstruct
 * the recorded ring, plus live counters. */
static void screen_record_stat_line(const char *id)
{
    int frames = 0, bytes = 0, dropped = 0, on = 0, scale = 1, ew = 0, eh = 0, cols = 0, fps = 0;
    char t[160]; SB b;
    uno_screen_capture_stat(&frames, &bytes, &dropped, &on, &scale, &ew, &eh, &cols, &fps);
    sb_init(&b, t, sizeof t);
    sb_s(&b, "frames "); sb_i(&b, frames); sb_s(&b, " bytes "); sb_i(&b, bytes);
    sb_s(&b, " dropped "); sb_i(&b, dropped); sb_s(&b, " ew "); sb_i(&b, ew);
    sb_s(&b, " eh "); sb_i(&b, eh); sb_s(&b, " cols "); sb_i(&b, cols);
    sb_s(&b, " tw "); sb_i(&b, UNO_SCREEN_TILE); sb_s(&b, " th "); sb_i(&b, UNO_SCREEN_TILE);
    sb_s(&b, " fps "); sb_i(&b, fps); sb_s(&b, " on "); sb_i(&b, on);
    t[b.len] = 0;
    rsp(id, "ok", t);
}

/* screen record start|stop|status|read - server-side session capture. The
 * device records keyframe+delta frames into a RAM ring on its shell tick (its
 * own snapshot, so it never disturbs the live view); the client stops it, reads
 * the ring (base64, like `screen read`), and reconstructs the frames. */
static void do_screen_record(const char *id, char *args)
{
    static unsigned char rbuf[SCREEN_READ_MAX];
    char *sub = tok(&args);
    if (!sub) { rsp(id, "err", "usage: screen record start|stop|status|read <off> [len]"); rsp(id, "end", 0); return; }

    if (!strcmp_(sub, "start")) {
        char *a1 = tok(&args), *a2 = tok(&args);
        int scale = a1 ? (int)atol_(a1) : 1;
        int fps   = a2 ? (int)atol_(a2) : 10;
        if (!uno_screen_capture_start(scale, fps)) {
            rsp(id, "err", "already recording (stop first)"); rsp(id, "end", 0); return;
        }
        screen_record_stat_line(id); rsp(id, "end", 0); return;
    }
    if (!strcmp_(sub, "stop"))   { uno_screen_capture_stop(); screen_record_stat_line(id); rsp(id, "end", 0); return; }
    if (!strcmp_(sub, "status")) { screen_record_stat_line(id); rsp(id, "end", 0); return; }
    if (!strcmp_(sub, "read")) {
        char *ao = tok(&args), *al = tok(&args);
        int off = ao ? (int)parse_hex(ao) : 0;
        int len = al ? (int)atol_(al) : SCREEN_READ_MAX;
        int n;
        if (len < 1) len = 1;
        if (len > SCREEN_READ_MAX) len = SCREEN_READ_MAX;
        n = uno_screen_capture_read(off, rbuf, len);
        if (n <= 0) { rsp(id, "err", "no-data (off past end / not recorded)"); rsp(id, "end", 0); return; }
        rsp_b64_stream(id, rbuf, n);
        rsp(id, "end", 0); return;
    }
    rsp(id, "err", "usage: screen record start|stop|status|read <off> [len]");
    rsp(id, "end", 0);
}

static void do_screen(const char *id, char *args)
{
    char *sub = tok(&args);
    int w = 0, h = 0;
    /* A security dialog is modal at the console.  `screen` is OBSERVE and so
     * passes the gate (the input-lock there only refuses DRIVE), but a screen
     * grab while a login / consent / accounts sheet is up captures exactly the
     * credentials being typed into it.  Mirror the DRIVE refusal for the
     * data-bearing subcommands (grab / read / record); `info` is only geometry,
     * so it stays available for a client that is just polling the link. */
    if (sub && uno_pc64_input_locked() &&
        (!strcmp_(sub, "grab") || !strcmp_(sub, "read") || !strcmp_(sub, "record"))) {
        rsp(id, "err", "refused (a security dialog is open at the console)");
        rsp(id, "end", 0); return;
    }
    if (!sub || !strcmp_(sub, "info")) {          /* `screen` / `screen info` */
        char t[48]; SB b;
        uno_screen_size(&w, &h);
        sb_init(&b, t, sizeof t);
        sb_i(&b, w); sb_c(&b, ' '); sb_i(&b, h); sb_s(&b, " rgba"); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0); return;
    }
    if (!strcmp_(sub, "grab")) {                  /* `screen grab [delta] [scale]` - stage it */
        char *a1 = tok(&args);
        int delta = 0, scale = 1;
        if (a1 && !strcmp_(a1, "delta")) { char *a2 = tok(&args); delta = 1; scale = a2 ? (int)atol_(a2) : 1; }
        else if (a1) scale = (int)atol_(a1);

        /* Delta: stage [QOI strip][manifest] of the changed tiles. A -1 falls
         * through to the full keyframe below (first grab, scale change, or a
         * strip too big) - so the client's one reader handles both headers. */
        if (delta) {
            int ew = 0, eh = 0, cols = 0, tw = 0, th = 0, nch = 0, strip = 0;
            int total = uno_screen_grab_delta(scale, g_screen, (int)sizeof g_screen,
                                              &ew, &eh, &cols, &tw, &th, &nch, &strip);
            if (total >= 0) {
                g_screen_len = total;
                {   /* delta <ew> <eh> <cols> <tw> <th> <nch> <strip> <total> */
                    char t[96]; SB b; sb_init(&b, t, sizeof t);
                    sb_s(&b, "delta "); sb_i(&b, ew); sb_c(&b, ' '); sb_i(&b, eh); sb_c(&b, ' ');
                    sb_i(&b, cols); sb_c(&b, ' '); sb_i(&b, tw); sb_c(&b, ' '); sb_i(&b, th); sb_c(&b, ' ');
                    sb_i(&b, nch); sb_c(&b, ' '); sb_i(&b, strip); sb_c(&b, ' '); sb_i(&b, total);
                    t[b.len] = 0; rsp(id, "ok", t);
                }
                rsp(id, "end", 0); return;
            }
            /* else: fall through and send a full keyframe */
        }

        {   /* full keyframe (also the delta fallback) */
            int n = uno_screen_grab_qoi(scale, g_screen, (int)sizeof g_screen, &w, &h);
            if (n < 0) { g_screen_len = 0; rsp(id, "err", "too-big (raise scale)"); rsp(id, "end", 0); return; }
            g_screen_len = n;
            {   /* header only: frame <w> <h> qoi <nbytes>; payload via `screen read` */
                char t[64]; SB b; sb_init(&b, t, sizeof t);
                sb_s(&b, "frame "); sb_i(&b, w); sb_c(&b, ' '); sb_i(&b, h);
                sb_s(&b, " qoi "); sb_i(&b, n); t[b.len] = 0;
                rsp(id, "ok", t);
            }
            rsp(id, "end", 0); return;
        }
    }
    if (!strcmp_(sub, "read")) {                  /* `screen read <off-hex> [len]` */
        char *ao = tok(&args), *al = tok(&args);
        long off = ao ? (long)parse_hex(ao) : 0;
        int len = al ? (int)atol_(al) : SCREEN_READ_MAX;
        if (g_screen_len <= 0) { rsp(id, "err", "no-frame (grab first)"); rsp(id, "end", 0); return; }
        if (off < 0 || off > g_screen_len) { rsp(id, "err", "bad-offset"); rsp(id, "end", 0); return; }
        if (len < 1) len = 1;
        if (len > SCREEN_READ_MAX) len = SCREEN_READ_MAX;
        if (off + len > g_screen_len) len = (int)(g_screen_len - off);
        rsp_b64_stream(id, g_screen + off, len);
        rsp(id, "end", 0); return;
    }
    if (!strcmp_(sub, "record")) { do_screen_record(id, args); return; }
    rsp(id, "err", "usage: screen [info|grab [delta] [scale]|read <off> [len]|record ...]");
    rsp(id, "end", 0);
}

/* `auth <token>` - the production handshake.  Until it succeeds the gate
 * refuses every other verb, so this is the only thing a fresh link can do.  A
 * debug build without `urc-auth` reports "open" and moves on, which is what
 * keeps every existing QEMU gate under tools/ working unchanged. */
static void do_auth(const char *id, char *args)
{
    char *tokv = tok(&args);
    if (!unoauto_gate_needs_auth()) {
        rsp(id, "ok", "open (debug build - no auth required)");
        rsp(id, "end", 0); return;
    }
    /* Throttle: at most AUTH_PER_DRAIN_MAX guesses are even evaluated per socket
     * read, so a pipelined burst cannot outrun the gate's lockout (item 1c). */
    if (++g_auth_in_drain > AUTH_PER_DRAIN_MAX) {
        rsp(id, "err", "auth failed"); rsp(id, "end", 0); return;
    }
    if (unoauto_gate_auth(tokv ? tokv : "")) {
        char t[96]; SB b; unsigned p = unoauto_gate_powers();
        sb_init(&b, t, sizeof t);
        sb_s(&b, "authenticated as "); sb_s(&b, unoauto_gate_owner_name());
        sb_s(&b, " powers=");
        if (p & UNOAUTO_P_OBSERVE) sb_s(&b, "observe ");
        if (p & UNOAUTO_P_DRIVE)   sb_s(&b, "drive ");
        if (p & UNOAUTO_P_SYSTEM)  sb_s(&b, "system");
        t[b.len] = 0;
        rsp(id, "ok", t);
    } else {
        /* Deliberately uninformative: "bad token" and nothing about how close
         * it was, how many tries remain, or whether the channel is even armed. */
        rsp(id, "err", "auth failed");
    }
    rsp(id, "end", 0);
}

/* `caps` - what may this link do?  A client asks once after `auth` and greys
 * out the rest of its UI, instead of discovering the boundary by tripping it. */
static void do_caps(const char *id)
{
    unsigned p = unoauto_gate_powers();
    rsp(id, "ok", (p & UNOAUTO_P_OBSERVE) ? "observe 1" : "observe 0");
    rsp(id, "ok", (p & UNOAUTO_P_DRIVE)   ? "drive 1"   : "drive 0");
    rsp(id, "ok", (p & UNOAUTO_P_SYSTEM)  ? "system 1"  : "system 0");
    rsp(id, "end", 0);
}

static void dispatch_cmd(const char *id, char *verb, char *args)
{
    const char *why = 0;
    if (!verb) { rsp(id, "err", "empty"); rsp(id, "end", 0); return; }

    /* THE GATE.  Everything below this point assumes the link is allowed to be
     * here; this is the one place that decides it.  `auth` runs before the
     * check (it IS the check's precondition); every other verb - including any
     * verb a later agent adds - is refused unless unoauto_gate.c has a row for
     * it and the console user granted that power.  Fail-closed by construction:
     * a new verb with no table row is denied, not ambient. */
    if (!strcmp_(verb, "auth")) { do_auth(id, args); return; }

    if (!unoauto_gate_verb(verb, &why)) {
        rsp(id, "err", why ? why : "denied");
        rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "caps")) { do_caps(id); return; }

    /* IMPLICIT CALL-HOME. Reaching here means a full inbound frame was received,
     * parsed and dispatched - proof the box is alive end to end - so refresh the
     * guard deadline for ANY command. A wedged command never returns to the
     * dispatch loop and so never reaches the next frame's refresh: that is
     * exactly what arms the reset. Refresh on receipt (before the handler runs)
     * so the risky verb about to execute gets its full timeout window. No-op
     * unless a guard is armed. See the `guard` verb + uno_dbg_guard_pet(). */
    uno_dbg_guard_pet();

    if (!strcmp_(verb, "probe")) { do_probe(id); return; }

    if (!strcmp_(verb, "log")) {
        unoauto_log(UA_CH_SCRIPT, "%s", args ? args : "");
        rsp(id, "ok", "logged"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "key")) {
        char *a1 = tok(&args), *a2 = tok(&args), *a3 = tok(&args);
        uno_pc64_inject_key((int)atol_(a1), (int)atol_(a2), a3 ? (int)atol_(a3) : 0);
        rsp(id, "ok", 0); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "pointer")) {
        char *a1 = tok(&args), *a2 = tok(&args), *a3 = tok(&args);
        uno_pc64_inject_pointer((int)atol_(a1), (int)atol_(a2), (int)atol_(a3));
        rsp(id, "ok", 0); rsp(id, "end", 0); return;
    }
    /* `apps` alone is the count, as it always was.  `apps list` names them: one
     * `id name` row per slot.  The registry means the SET of apps now depends on
     * what is installed rather than on the build, so a caller that wants to
     * drive one has to be able to look it up. */
    if (!strcmp_(verb, "apps")) {
        char *a1 = tok(&args);
        if (a1 && !strcmp_(a1, "list")) {
            int i, n = pc64_shell_app_count();
            for (i = 0; i < n; i++) {
                char t[64]; SB b; sb_init(&b, t, sizeof t);
                sb_s(&b, pc64_shell_app_id(i)); sb_s(&b, " ");
                sb_s(&b, pc64_shell_app_name(i)); t[b.len] = 0;
                rsp(id, "ok", t);
            }
            rsp(id, "end", 0); return;
        }
        { char t[16]; SB b; sb_init(&b, t, sizeof t); sb_i(&b, pc64_shell_app_count()); t[b.len] = 0;
          rsp(id, "ok", t); rsp(id, "end", 0); return; }
    }
    /* `launch <n>` or `launch <id>`.  The id form is the one to use: a slot
     * index is this boot's ordering of whatever is installed, so a test that
     * launches by number does not FAIL when an app is added, it quietly drives
     * a different app.  An argument starting with a digit is a number, and an
     * id can never start with one. */
    if (!strcmp_(verb, "launch")) {
        char *a1 = tok(&args);
        int a = (a1 && *a1 >= '0' && *a1 <= '9') ? (int)atol_(a1)
              : (a1 ? pc64_shell_app_by_id(a1) : -1);
        int ok = pc64_shell_launch(a);
        rsp(id, ok ? "ok" : "err", ok ? "launched" : "no-app"); rsp(id, "end", 0); return;
    }
    /* pick up a .UNO that has landed since boot (a `push` into APPS\, an
     * install, a stick) without a reboot */
    if (!strcmp_(verb, "rescan")) {
        char t[16]; SB b;
        pc64_shell_apps_rescan();
        sb_init(&b, t, sizeof t); sb_i(&b, pc64_shell_app_count()); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "close")) {
        pc64_shell_close_top(); rsp(id, "ok", 0); rsp(id, "end", 0); return;
    }
    /* screen [info|grab [delta] [scale]|read <off> [len]] - the OUT half of
     * remote desktop: QOI-encode the framebuffer (whole, or only the changed
     * tiles with `grab delta`) and stream it base64 (like readsec). Read-only,
     * no arm gate. Pairs with key/pointer (the IN half). See unoauto_screen.c. */
    if (!strcmp_(verb, "screen")) { do_screen(id, args); return; }
    /* stream start|stop|status - unostream: the guest dials a host receiver
     * and pushes QOI keyframe/delta frames live on its own socket + tick (demo
     * video capture; see pc64/UNOSTREAM.md). OBSERVE, like `screen`. Additive
     * pass-through to unostream_cmd (weak-stubbed above). */
    if (!strcmp_(verb, "stream")) {
        static char none[1];                  /* tokenised in place: no literal */
        int n = unostream_cmd(args ? args : none, g_report, (int)sizeof g_report);
        rsp(id, n >= 0 ? "ok" : "err", g_report);
        rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "uptime")) {
        char t[24]; SB b; sb_init(&b, t, sizeof t); sb_i(&b, (long)uno_dbg_uptime_ms()); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "poweroff")) {
        g_pending_off = 1; rsp(id, "ok", "bye"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "reboot")) {
        g_pending_reboot = 1; rsp(id, "ok", "bye"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "put"))  { do_put(id, args); return; }
    if (!strcmp_(verb, "vols")) { do_vols(id); return; }
    /* raw-disk authoring (partition/format disk B) - see the do_* wrappers */
    if (!strcmp_(verb, "disks"))   { do_disks(id); return; }
    if (!strcmp_(verb, "arm"))     { do_arm(id, (int)atol_(tok(&args))); return; }
    if (!strcmp_(verb, "disarm"))  { g_armed_disk = -1; rsp(id, "ok", "disarmed"); rsp(id, "end", 0); return; }
    if (!strcmp_(verb, "readsec")) { do_readsec(id, args); return; }
    if (!strcmp_(verb, "writesec")){ do_writesec(id, args); return; }
    if (!strcmp_(verb, "gptinit")) { do_gptinit(id, args); return; }
    if (!strcmp_(verb, "mkpart"))  { do_mkpart(id, args); return; }
    if (!strcmp_(verb, "mkfs"))    { do_mkfs(id, args); return; }
    if (!strcmp_(verb, "prepdisk")){ do_prepdisk(id, args); return; }
    if (!strcmp_(verb, "makeboot")){ do_makeboot(id, args); return; }
    if (!strcmp_(verb, "mkdir"))   { do_mkdir(id, args); return; }
    if (!strcmp_(verb, "install")) { do_install(id, args); return; }
    /* iwl <subcmd...> - live Intel-WiFi register/bring-up debug (F12). See
     * iwlwifi.h iwl_dbg_cmd: csr/csw/prr/prw/rerun/status. Additive pass-through
     * per the 2026-07-22 request in UNOAUTOMATE-REQUESTS.md. */
    if (!strcmp_(verb, "iwl")) {
        int n = iwl_dbg_cmd(args ? args : "", g_report, (int)sizeof g_report);
        rsp(id, n >= 0 ? "ok" : "err", n >= 0 ? g_report : "bad-cmd (csr/csw/prr/prw/rerun/status)");
        rsp(id, "end", 0); return;
    }
    /* eth <subcmd...> - live wired-NIC (Realtek r8169) register/bring-up debug,
     * the wired sibling of `iwl`. Additive pass-through to r8169_dbg_cmd (r8169.c;
     * weak-stubbed above until the driver lands it). Subcmds:
     * status/reg/wreg/phy/wphy/rerun/link/mac. Per the 2026-07-22 r8169 request
     * in UNOAUTOMATE-REQUESTS.md. */
    if (!strcmp_(verb, "eth")) {
        int n = r8169_dbg_cmd(args ? args : "", g_report, (int)sizeof g_report);
        rsp(id, n >= 0 ? "ok" : "err",
            n >= 0 ? g_report : "bad-cmd (status/reg/wreg/phy/wphy/rerun/link/mac)");
        rsp(id, "end", 0); return;
    }
    /* hwwdt <subcmd...> - PCH TCO hardware watchdog (unodevices, uno_hw_wdt.c),
     * the guard's IRQs-off backstop. Additive pass-through to uno_hw_wdt_cmd
     * (weak-stubbed above until the module lands). Subcmds:
     *   status               present/gen/TCOBASE + raw GEN_PMCON_A (fw=0x..) dump
     *   arm <s> / pet / disarm   drive the TCO directly (no cli-spin - SAFE: an
     *                        armed-but-unpetted TCO resets the box in ~<s>, and
     *                        if NO_REBOOT wasn't really cleared it simply doesn't,
     *                        so this never hard-hangs the box)
     *   selftest <s> / wedge     cli-spin (IRQs-off) - the metal wedge trigger;
     *                        NEVER RETURNS, only the TCO recovers. UNO_DEBUG-only. */
    if (!strcmp_(verb, "hwwdt")) {
        int n = uno_hw_wdt_cmd(args ? args : "status", g_report, (int)sizeof g_report);
        rsp(id, n >= 0 ? "ok" : "err",
            n >= 0 ? g_report : "bad-cmd (status/arm/pet/disarm/selftest/wedge)");
        rsp(id, "end", 0); return;
    }
    /* devices - read-only PCI device-tree listing, one `ok` line per device.
     * Pure pass-through to unodevices' devmgr_list_str (weak-stubbed above until
     * that subsystem lands); mutates nothing, so no `arm` gate. The line FORMAT
     * is unodevices' to define, not ours - we only split its dump on newlines,
     * so a phase-2 driver/UNCLAIMED column appears here with no change to this
     * file. Per the 2026-07-23 planning-agent request in UNOAUTOMATE-REQUESTS.md;
     * feeds the detach-completion plan's "what is still unclaimed on this box?"
     * query on headless machines. */
    if (!strcmp_(verb, "devices")) {
        int n = devmgr_list_str(g_report, (int)sizeof g_report);
        char *p = g_report;
        if (n < 0) { rsp(id, "err", g_report); rsp(id, "end", 0); return; }
        while (*p) {                              /* stream the listing by line */
            char *nl = p; while (*nl && *nl != '\n') nl++;
            { char save = *nl; *nl = 0; if (*p) rsp(id, "ok", p); *nl = save; }
            if (!*nl) break;
            p = nl + 1;
        }
        rsp(id, "end", 0); return;
    }
    /* skin [status|list|load <vol> <file.wsz>|scan|off] - re-skin a RUNNING
     * UnoAmp and repaint it, so a skin change can be shown without a reboot
     * (it used to be a one-shot at player open). Additive pass-through to
     * unoamp_skin_cmd (weak-stubbed above). DRIVE, not OBSERVE: it changes
     * what is on the screen, exactly like `launch`. */
    if (!strcmp_(verb, "skin")) {
        static char none[1];                  /* tokenised in place: no literal */
        int n = unoamp_skin_cmd(args ? args : none, g_report, (int)sizeof g_report);
        rsp(id, n >= 0 ? "ok" : "err", g_report);
        rsp(id, "end", 0); return;
    }
    /* ssh [keys|keygen|keypub|keyrm|sess|sessadd|sessrm|hosts|run|get|close]
     * - the SSH client's own sub-verb grammar, verbatim; the output format is
     * unossh's and does not come back here. SYSTEM, not DRIVE: it generates
     * and stores PRIVATE KEYS, writes them to disk, and runs commands on other
     * machines with this box's credentials - a blast radius past this machine,
     * which is the whole reason the tier exists. Same-commit GATE[] row. */
    if (!strcmp_(verb, "ssh")) {
        int n = ssh_dbg_cmd(args ? args : "", g_report, (int)sizeof g_report);
        rsp(id, n >= 0 ? "ok" : "err", n >= 0 ? g_report : "bad-cmd (try: ssh help)");
        rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "bootnext")) {
        int ok = uno_pc64_set_bootnext((unsigned)atol_(tok(&args)));
        rsp(id, ok ? "ok" : "err",
            ok ? "set" : "unavailable (detached / no runtime SetVariable)");
        rsp(id, "end", 0); return;
    }
    /* guard <timeout-s> [reboot] - arm the host-attested dead-man's switch
     * BEFORE a risky verb (e.g. `iwl mvm` then `iwl rerun` into never-run
     * firmware). If the box can't service an inbound URC command within
     * <timeout-s> - the signature of a wedge that stops it from calling home -
     * the debug watchdog ISR hard-resets it and it re-dials on its own. Any
     * later inbound command refreshes the deadline (the implicit pet at the top
     * of dispatch); `safe` stands it down once the op returns. v1: reboot is the
     * only action; an explicit "reboot" word is accepted and ignored so the
     * arg slot is stable for future actions (e.g. revert). */
    if (!strcmp_(verb, "guard")) {
        int secs = (int)atol_(tok(&args));
        if (secs <= 0) {
            rsp(id, "err", "usage: guard <timeout-s> [reboot]"); rsp(id, "end", 0); return;
        }
        g_guard_token = g_guard_token * 1664525u + 1013904223u
                        + (unsigned)uno_dbg_uptime_ms();
        g_guard_token_minted = 1;               /* `safe` now requires this token */
        uno_dbg_guard_arm((unsigned)secs * 1000u);
        {
            char t[64]; SB b; sb_init(&b, t, sizeof t);
            sb_s(&b, "armed "); sb_i(&b, secs);
            sb_s(&b, "s action=reboot token="); sb_i(&b, (long)g_guard_token);
            t[b.len] = 0; rsp(id, "ok", t);
        }
        unoauto_log(UA_CH_SCRIPT, "guard armed %ds (token %u)", secs, g_guard_token);
        rsp(id, "end", 0); return;
    }
    /* pet - explicit keep-alive for a legitimately long op. The refresh already
     * happened implicitly at the top of dispatch; this just reports state so a
     * host keep-alive loop has something to poll. */
    if (!strcmp_(verb, "pet")) {
        rsp(id, "ok", uno_dbg_guard_armed() ? "petted" : "not-armed");
        rsp(id, "end", 0); return;
    }
    /* safe [token] - disarm; the guarded op returned, stand down. Once a token
     * has been minted by `guard`, `safe` MUST present it: a tokenless disarm
     * used to clear any live guard, so a stale (or hostile) `safe` from another
     * session could stand a fresh guard down. Require the match whenever a token
     * exists; only a guard that never issued one accepts a bare `safe`. */
    if (!strcmp_(verb, "safe")) {
        char *a = tok(&args);
        if (g_guard_token_minted) {
            if (!a) { rsp(id, "err", "token-required"); rsp(id, "end", 0); return; }
            if ((unsigned)atol_(a) != g_guard_token) {
                rsp(id, "err", "bad-token"); rsp(id, "end", 0); return;
            }
        }
        uno_dbg_guard_clear();
        rsp(id, "ok", "disarmed"); rsp(id, "end", 0); return;
    }
#ifdef UNO_DEBUG
    /* nst and disc are pure self-test verbs (netsock / discovery harnesses driven
     * by tools/*.py). They have no place in a shipped image, so they compile out
     * entirely in production - and their GATE rows are #ifdef'd out too, so even
     * naming one on a production link is refused as unknown rather than reachable. */
    /* nst <p1> <p2> - netsock self-test (debug): prove the multi-connection
     * layer. Open TWO simultaneous outbound TCP connections (to 10.0.2.2:p1 and
     * :p2), plus a LISTEN socket on 9099 that accepts one inbound connection
     * (the host dials in via QEMU hostfwd). Reports socket count, both outbound
     * states, and the accepted child + its peer. Driven by tools/netsock_qemu.py. */
    if (!strcmp_(verb, "nst")) {
        extern void uno_pc64_delay_ms(int ms);
        int p1 = (int)atol_(tok(&args));
        int p2 = (int)atol_(tok(&args));
        u8  host[4] = {10, 0, 2, 2};
        int sA = net_socket(SOCK_TCP), sB = net_socket(SOCK_TCP), sL = net_socket(SOCK_TCP);
        int child = -1, i;
        char t[96]; SB b;
        if (p1 > 0) net_connect(sA, host, (u16)p1);
        if (p2 > 0) net_connect(sB, host, (u16)p2);
        net_bind(sL, 9099); net_listen(sL);
        for (i = 0; i < 400; i++) {                 /* ~2 s: settle handshakes + accept */
            net_poll(); uno_pc64_delay_ms(5);
            if (child < 0) { int c = net_accept(sL); if (c >= 0) child = c; }
        }
        sb_init(&b, t, sizeof t); sb_s(&b, "count=");  sb_i(&b, net_sock_count());   t[b.len]=0; rsp(id,"ok",t);
        sb_init(&b, t, sizeof t); sb_s(&b, "connA=");  sb_i(&b, net_sock_state(sA));  t[b.len]=0; rsp(id,"ok",t);
        sb_init(&b, t, sizeof t); sb_s(&b, "connB=");  sb_i(&b, net_sock_state(sB));  t[b.len]=0; rsp(id,"ok",t);
        sb_init(&b, t, sizeof t); sb_s(&b, "accepted="); sb_i(&b, child);
        if (child >= 0) {
            u8 pip[4]; u16 pp; net_sock_peer(child, pip, &pp);
            sb_s(&b, " peer="); sb_i(&b, pip[0]); sb_c(&b,'.'); sb_i(&b, pip[1]);
            sb_c(&b,'.'); sb_i(&b, pip[2]); sb_c(&b,'.'); sb_i(&b, pip[3]); sb_c(&b,':'); sb_i(&b, pp);
        }
        t[b.len]=0; rsp(id,"ok",t);
        net_sock_close(sA); net_sock_close(sB);
        if (child >= 0) net_sock_close(child);
        net_sock_close(sL);
        rsp(id, "end", 0); return;
    }
    /* disc - report zero-config discovery state to the dev PC (query only). The
     * discovery machinery is armed by the DEBUG.CFG `discover` flag and pumped
     * in netdisc_tick; this lets a host tool ask "is discovery armed, did pc64
     * record my OFFER, and what host:port did it latch?" without watching the
     * wire. link= echoes the remote-channel state (RS_UP=3 here, since we only
     * dispatch on an established link). Driven by tools/netdisc_qemu.py. */
    if (!strcmp_(verb, "disc")) {
        char t[80]; SB b;
        sb_init(&b, t, sizeof t); sb_s(&b, "active=");    sb_i(&b, netdisc_active());    t[b.len]=0; rsp(id,"ok",t);
        sb_init(&b, t, sizeof t); sb_s(&b, "have_host="); sb_i(&b, netdisc_have_host()); t[b.len]=0; rsp(id,"ok",t);
        if (netdisc_have_host()) {
            const u8 *hip = netdisc_host_ip();
            sb_init(&b, t, sizeof t); sb_s(&b, "host=");
            sb_i(&b, hip[0]); sb_c(&b,'.'); sb_i(&b, hip[1]); sb_c(&b,'.');
            sb_i(&b, hip[2]); sb_c(&b,'.'); sb_i(&b, hip[3]); sb_c(&b,':');
            sb_i(&b, netdisc_host_port()); t[b.len]=0; rsp(id,"ok",t);
        }
        sb_init(&b, t, sizeof t); sb_s(&b, "link=");       sb_i(&b, g_state);            t[b.len]=0; rsp(id,"ok",t);
        rsp(id, "end", 0); return;
    }
#endif /* UNO_DEBUG - nst / disc */
    if (!strcmp_(verb, "test")) {
        do_test(id, tok(&args)); return;
    }
    if (!strcmp_(verb, "py")) {
        /* Run under the link's REMOTE-trust session, not whatever INTERACTIVE
         * session the shell has bound.  pc64_shell_py_exec binds no identity of
         * its own, so without this the remote `py` would execute as the console
         * user - REMOTE-trust restrictions and the arming user's uid are what
         * should apply to code that arrived over the wire.  Enter the link
         * session around the call; leave restores the prior binding. */
        usec_session_t ls = unoauto_gate_link_session();
        int entered = (ls != 0) && unosec_enter_session(ls);
        int rc = pc64_shell_py_exec(args ? args : "", g_report, (int)sizeof g_report);
        char *p = g_report;
        if (entered) unosec_leave();
        while (*p) {                              /* stream captured output */
            char *nl = p; while (*nl && *nl != '\n') nl++;
            { char save = *nl; *nl = 0; rsp(id, rc == 0 ? "ok" : "err", p); *nl = save; }
            if (!*nl) break;
            p = nl + 1;
        }
        if (!g_report[0]) rsp(id, rc == 0 ? "ok" : "err", rc == 0 ? "" : "error");
        rsp(id, "end", 0); return;
    }
    rsp(id, "err", "unknown-verb"); rsp(id, "end", 0);
}

static void dispatch_line(char *line)
{
    char *type = tok(&line);
    if (!type) return;
    if (!strcmp_(type, "CMD")) {
        char *id = tok(&line);
        char *verb = tok(&line);
        skip_ws(&line);
        if (!id) return;
        dispatch_cmd(id, verb, line);
    } else if (!strcmp_(type, "MSG")) {
        skip_ws(&line);
        inq_push(line);
    } else if (!strcmp_(type, "HELLO")) {
        /* peer greeting; nothing required */
    } else if (!strcmp_(type, "BYE")) {
        unoauto_remote_stop();
    }
    /* RSP frames (responses to pc64-initiated CMDs) are surfaced to scripts
     * as MSG-style inbound lines too, so a script can correlate them. */
    else if (!strcmp_(type, "RSP")) { skip_ws(&line); inq_push(line); }
}

/* ---- transport seam ------------------------------------------------------
 * Everything above (URC framing, dispatch, the tx/rx queues) is transport-
 * agnostic; only these ops touch a medium.  Two backends implement it:
 *   - TCP    : the dev-PC LAN link (pc64's own socket, via netsock).
 *   - serial : a 16550 UART (unoauto_serial.c), so a box whose only network is
 *              the NIC being debugged can still be driven live over URC
 *              (Request 2, 2026-07-22 r8169 entry in UNOAUTOMATE-REQUESTS.md).
 * A backend reports a coarse link state; the pump's state machine is written
 * against these, not against TCP_* directly. */
enum { LINK_CLOSED = 0, LINK_CONNECTING, LINK_UP, LINK_DEAD };
typedef struct {
    const char *name;
    int  (*medium_up)(void);                    /* physical medium ready to try */
    int  (*open)(void);                         /* begin a link; 0 ok, <0 fail  */
    int  (*state)(void);                        /* LINK_*                       */
    int  (*send)(const char *buf, int n);       /* bytes accepted (>= 0)        */
    int  (*recv)(unsigned char *buf, int cap);  /* bytes read (>= 0)            */
    void (*close)(void);
    void (*poll)(void);                         /* pump the lower layer         */
} urc_transport;

/* --- TCP backend (the original medium) --- */
static int  tcp_medium_up(void) { return pc64_net_up(); }
static int  tcp_open(void)
{
    if (g_sock >= 0) net_sock_close(g_sock);           /* free a prior attempt */
    g_sock = net_socket(SOCK_TCP);
    if (g_sock < 0) return -1;
    net_connect(g_sock, g_ip, g_port);
    return 0;
}
static int  tcp_state(void)
{
    int st = (g_sock >= 0) ? net_sock_state(g_sock) : TCP_CLOSED;
    if (st == TCP_ESTABLISHED)              return LINK_UP;
    if (st == TCP_CLOSED || st == TCP_DONE) return LINK_DEAD;
    return LINK_CONNECTING;
}
static int  tcp_send(const char *b, int n)    { int r = net_send(g_sock, b, n); return r > 0 ? r : 0; }
static int  tcp_recv(unsigned char *b, int c) { int r = net_recv(g_sock, b, c); return r > 0 ? r : 0; }
static void tcp_close(void)                   { if (g_sock >= 0) { net_sock_close(g_sock); g_sock = -1; } }
static void tcp_poll(void)                    { net_poll(); }
static const urc_transport TP_TCP = {
    "tcp", tcp_medium_up, tcp_open, tcp_state, tcp_send, tcp_recv, tcp_close, tcp_poll
};

/* --- TCP LISTEN backend: URC as a SERVER (the dev PC dials INTO the box) ---
 * With `listen` in DEBUG.CFG the box binds+listens on g_port (netsock
 * net_listen) and accepts one inbound URC connection, then speaks the identical
 * line protocol over it. The listener PERSISTS across client reconnects, so
 * unlike dial-out there is no connect timeout and no remote to retry - a dropped
 * client just drops us back to waiting for the next accept. send/recv/poll are
 * the TCP backend's (they act on g_sock = the accepted child). */
static int  lst_medium_up(void) { return pc64_net_up(); }   /* need the NIC + an IP to bind */
static int  lst_open(void)
{
    if (g_listen_sock < 0) {                        /* create the listener once */
        g_listen_sock = net_socket(SOCK_TCP);
        if (g_listen_sock < 0) return -1;
        if (net_bind(g_listen_sock, g_port) < 0) { net_sock_close(g_listen_sock); g_listen_sock = -1; return -1; }
        if (net_listen(g_listen_sock) < 0)       { net_sock_close(g_listen_sock); g_listen_sock = -1; return -1; }
    }
    if (g_sock >= 0) { net_sock_close(g_sock); g_sock = -1; }   /* drop any prior child */
    return 0;
}
/* Say what the listener is doing. Nothing announced accept or disconnect, so
 * from the client end a wedged listener and a crashed box looked identical -
 * the second half of the listen-mode request (2026-08-04). One line each way
 * is enough to tell them apart in seconds. */
static void lst_log(const char *what, int sock)
{
    unsigned char ip[4]; unsigned short port = 0;
    if (sock >= 0 && net_sock_peer(sock, ip, &port) == 0)
        ulog_notice(LF_KERNEL, "remote: %s %u.%u.%u.%u:%u (listening on %u)",
                    what, ip[0], ip[1], ip[2], ip[3], port, (unsigned)g_port);
    else
        ulog_notice(LF_KERNEL, "remote: %s (listening on %u)", what, (unsigned)g_port);
}

static int  lst_state(void)
{
    if (g_sock >= 0) {                              /* a client is connected */
        int st = net_sock_state(g_sock);
        if (st == TCP_ESTABLISHED) return LINK_UP;
        lst_log("client gone, listening again", g_sock);
        net_sock_close(g_sock); g_sock = -1;        /* it left - report the drop and */
        return LINK_CONNECTING;                      /* accept the next one on a later tick */
    }
    if (g_listen_sock >= 0) {                       /* try to accept a dial-in */
        int c = net_accept(g_listen_sock);
        if (c >= 0) {                               /* freshly-established child */
            g_sock = c;
            lst_log("client accepted", c);
            return LINK_UP;
        }
    }
    return LINK_CONNECTING;                          /* still waiting for a client */
}
static void lst_close(void) { if (g_sock >= 0) { net_sock_close(g_sock); g_sock = -1; } }
static const urc_transport TP_TCP_LISTEN = {
    "listen", lst_medium_up, lst_open, lst_state, tcp_send, tcp_recv, lst_close, tcp_poll
};

/* --- serial backend (16550 UART, unoauto_serial.c) ---
 * The UART is a point-to-point wire with no handshake: it is "up" as soon as it
 * is initialised.  The URC HELLO handshake still runs at the line-protocol
 * layer; if the host attaches mid-stream it just resyncs on the next newline. */
static int  ser_medium_up(void) { return 1; }
static int  ser_open(void)      { uart_init(g_uart_base, 115200); return 0; }
static int  ser_state(void)     { return LINK_UP; }
static void ser_close(void)     { }
static void ser_poll(void)      { }
static const urc_transport TP_SERIAL = {
    "serial", ser_medium_up, ser_open, ser_state, uart_write, uart_read, ser_close, ser_poll
};

static const urc_transport *g_tp = &TP_TCP;   /* default; boot may pick serial */

/* ---- pump ---------------------------------------------------------------- */
static void flush_tx(void)
{
    int n, r;
    if (g_txlen <= 0) return;
    n = g_txlen; if (n > 512) n = 512;
    r = g_tp->send(g_tx, n);             /* one segment / FIFO-load at a time */
    if (r > 0) {
        g_txlen -= r;
        if (g_txlen > 0) memmove(g_tx, g_tx + r, (unsigned long)g_txlen);
    }
}

static void drain_rx(void)
{
    unsigned char buf[512];
    int n, i;
    g_auth_in_drain = 0;                 /* fresh auth budget per pass (item 1c) */
    while ((n = g_tp->recv(buf, (int)sizeof buf)) > 0) {
        for (i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') { g_rx[g_rxlen] = 0; dispatch_line(g_rx); g_rxlen = 0; g_rx_seen = 1; }
            else if (g_rxlen < (int)sizeof g_rx - 1) g_rx[g_rxlen++] = c;
            /* else: overline - drop chars until the next '\n' */
        }
    }
}

static void start_connect(void)
{
    g_txlen = 0; g_rxlen = 0;
    if (!g_tp->medium_up()) { g_state = RS_DOWN; g_deadline = g_tick + 300; return; }
    if (g_tp->open() < 0)   { g_state = RS_DOWN; g_deadline = g_tick + 300; return; }
    g_state = RS_CONNECTING;
    g_deadline = g_tick + 600;           /* ~10 s handshake window */
}

void unoauto_remote_boot(void)
{
    char v[64]; char *p; int oct, i;
    if (g_state != RS_OFF) return;                 /* armed once */

    unoauto_gate_boot();       /* debug `urc-auth=<token>` hook; no-op in prod */

    /* PRODUCTION: nothing happens until a console user arms the channel.  This
     * one line is the difference between "a shipped OS that can be remotely
     * driven if you ask it to" and "a shipped OS listening on the LAN".  The
     * gate is open unconditionally in a debug build, so the harness path below
     * is untouched. */
    if (!unoauto_gate_open()) return;

    /* URC SERVER mode: `listen` (bare = port 5099) or `listen=<port>` makes the
     * box a LISTENER - the dev PC dials INTO it (netsock net_listen/net_accept),
     * instead of the box dialing out. Also arms netdisc as a responder so a
     * scanning client can discover the box and its listen port. Mutually
     * exclusive with remote=/discover/remote-serial (all of which dial out);
     * checked first. */
    if (pc64_stress_cfg_flag("listen") > 0) {
        char lb[16];
        int lp = (pc64_stress_cfg_value("listen", lb, (int)sizeof lb) > 0) ? (int)atol_(lb) : 5099;
        if (lp <= 0 || lp > 65535) lp = 5099;
        g_port = (u16)lp;
        g_listening = 1;
        g_tp = &TP_TCP_LISTEN;
        if (g_sink < 0)
            g_sink = unoauto_sink_add((1u << UA_CH_COUNT) - 1, remote_sink, 0);
        netdisc_listen((unsigned short)lp);        /* answer scans with our ip:port */
        unoauto_log(UA_CH_SCRIPT, "remote: listening for a dev-PC dial-in on :%d", lp);
        start_connect();                            /* binds+listens; then awaits accept */
        return;
    }

    /* NIC-independent transport (Request 2): a 16550 UART link, for a box whose
     * only network is the NIC being debugged.  `remote-serial` (bare flag) uses
     * COM1 @ 115200; `remote-serial=<hexbase>` picks another UART (e.g. `2f8` =
     * COM2).  Checked before `remote=` - a serial-only stick has no IP address,
     * and the UART is up with no handshake, so we go straight to connecting. */
    if (pc64_stress_cfg_flag("remote-serial") > 0) {
        char sb[16];
        g_uart_base = (pc64_stress_cfg_value("remote-serial", sb, (int)sizeof sb) > 0)
                      ? (unsigned)parse_hex(sb) : 0x3F8;
        g_tp = &TP_SERIAL;
        if (g_sink < 0)
            g_sink = unoauto_sink_add((1u << UA_CH_COUNT) - 1, remote_sink, 0);
        unoauto_log(UA_CH_SCRIPT, "remote: serial link on 0x%x", g_uart_base);
        start_connect();
        return;
    }

    if (pc64_stress_cfg_value("remote", v, (int)sizeof v) <= 0) {
        /* No static address. If discovery is armed (`discover` flag), wait for
         * netdisc to find a host and dial it - zero-config. */
        if (pc64_stress_cfg_flag("discover") > 0) {
            if (g_sink < 0)
                g_sink = unoauto_sink_add((1u << UA_CH_COUNT) - 1, remote_sink, 0);
            g_state = RS_DISCOVER;
            unoauto_log(UA_CH_SCRIPT, "remote: awaiting discovery (no remote= key)");
            return;
        }
        /* PRODUCTION default: no DEBUG.CFG exists, so every key above read as
         * absent and we land here with the gate armed.  LISTEN is the right
         * shape for a shipped machine - the operator arms it at the console,
         * reads the token off the screen, and dials IN from wherever they are.
         * Dial-out would need a host address nobody has typed. */
        if (unoauto_gate_armed()) {
            g_port = 5099;
            g_listening = 1;
            g_tp = &TP_TCP_LISTEN;
            if (g_sink < 0)
                g_sink = unoauto_sink_add((1u << UA_CH_COUNT) - 1, remote_sink, 0);
            /* Deliberately NOT netdisc_listen() here (item 2).  Auto-advertising
             * on the LAN whenever the box is armed lets any unauthenticated
             * scanner enumerate it and its listen port.  The operator already has
             * the address AND the PIN off the Remote Control panel, so discovery
             * buys nothing in production and only widens the pre-auth surface -
             * the responder is opt-in via the explicit `listen` DEBUG.CFG path
             * (a dev/debug choice), off by default on a shipped machine. */
            unoauto_log(UA_CH_SCRIPT, "remote: armed, listening on :5099 (discovery off)");
            start_connect();
        }
        return;
    }
    /* parse a.b.c.d:port */
    p = v;
    for (i = 0; i < 4; i++) {
        oct = (int)atol_(p);
        g_ip[i] = (u8)oct;
        while (*p && *p != '.' && *p != ':') p++;
        if (i < 3) { if (*p != '.') { goto bad; } p++; }
    }
    if (*p != ':') goto bad;
    p++;
    g_port = (u16)atol_(p);
    if (!g_port) goto bad;
    if (g_sink < 0)
        g_sink = unoauto_sink_add((1u << UA_CH_COUNT) - 1, remote_sink, 0);
    unoauto_log(UA_CH_SCRIPT, "remote: dialing %d.%d.%d.%d:%d",
                g_ip[0], g_ip[1], g_ip[2], g_ip[3], g_port);
    start_connect();
    return;
bad:
    unoauto_log(UA_CH_SCRIPT, "remote: bad address '%s' in DEBUG.CFG", v);
}

void unoauto_remote_tick(void)
{
    int ls;
    if (g_state == RS_OFF) return;
    g_tick++;
    g_tp->poll();
    if (g_state == RS_DISCOVER) {                    /* waiting for a discovered host */
        if (netdisc_have_host()) {
            const u8 *hip = netdisc_host_ip();
            g_ip[0] = hip[0]; g_ip[1] = hip[1]; g_ip[2] = hip[2]; g_ip[3] = hip[3];
            g_port = netdisc_host_port();
            unoauto_log(UA_CH_SCRIPT, "remote: discovered %d.%d.%d.%d:%d",
                        g_ip[0], g_ip[1], g_ip[2], g_ip[3], g_port);
            start_connect();
        }
        return;
    }
    ls = g_tp->state();
    switch (g_state) {
    case RS_CONNECTING:
        if (ls == LINK_UP) {
            g_state = RS_UP;
            g_rx_seen = 0; g_hello_at = g_tick + 120;   /* ~2 s re-HELLO cadence */
            unoauto_remote_send("HELLO", "pc64 1");
            /* now that the sink is live, announce the link on the SCRIPT
             * channel - this line flows straight back out as a LOG frame,
             * seeding the remote-log stream. */
            unoauto_log(UA_CH_SCRIPT, "remote: link up");
        } else if (!g_listening && (ls == LINK_DEAD || g_tick > g_deadline)) {
            g_tp->close();
            g_state = RS_DOWN; g_deadline = g_tick + 300;   /* retry ~5 s */
        }
        /* listen mode: no timeout - stay here polling accept until a host dials in */
        break;
    case RS_UP:
        drain_rx();
        /* A verb can stand the whole channel down under us - `disarm`, or three
         * failed auths tripping the gate - and drain_rx runs the dispatcher.
         * Bail rather than pumping a link that no longer exists. */
        if (g_state != RS_UP) break;
        /* Serial has no connection handshake, so a host that attaches AFTER the
         * guest booted never saw the one link-up HELLO.  Re-emit it every ~2 s
         * until we hear the host, so a late `unoauto_remote.py --serial` syncs. */
        if (g_tp == &TP_SERIAL && !g_rx_seen && g_tick >= g_hello_at) {
            unoauto_remote_send("HELLO", "pc64 1");
            g_hello_at = g_tick + 120;
        }
        flush_tx();
        if (ls != LINK_UP) {
            g_tp->close();
            /* The client left.  Deauthenticate: the NEXT dial-in is a different
             * peer as far as we know, and must present the token itself.  The
             * channel stays armed - in listen mode the operator expects to
             * reconnect without walking back to the machine. */
            unoauto_gate_link_reset();
            if (g_listening) {                  /* keep the listener; await a re-dial */
                g_txlen = 0; g_rxlen = 0; g_rx_seen = 0;
                g_state = RS_CONNECTING; g_deadline = g_tick + 600;
            } else {
                g_state = RS_DOWN; g_deadline = g_tick + 300;
            }
        } else if (g_pending_off && g_txlen == 0) {
            uno_fat_sync();                     /* flush write-back FAT lines so
                                                   remote put/mkdir writes reach
                                                   the disk before we power off */
            uno_pc64_shutdown();
        } else if (g_pending_reboot && g_txlen == 0) {
            uno_fat_sync();                     /* same: persist before reset    */
            uno_native_reset();                 /* never returns */
        }
        break;
    case RS_DOWN:
        if (g_tick >= g_deadline) start_connect();
        break;
    default: break;
    }
}

int unoauto_remote_active(void) { return g_state == RS_UP; }

void unoauto_remote_stop(void)
{
    if (g_state == RS_UP) { unoauto_remote_send("BYE", 0); flush_tx(); }
    g_tp->close();
    g_state = RS_OFF;
    g_txlen = 0; g_rxlen = 0;
}

