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
#include "net.h"            /* u8/u16, net_tcp_*, net_poll */
#include "netsock.h"        /* multi-connection socket API (own link socket) */
#include "netdisc.h"        /* zero-config discovery: auto-dial the found host */
#include "pc64_http.h"      /* pc64_net_up */
#include "iwlwifi.h"        /* iwl_dbg_cmd - the `iwl` verb (F12 live debug) */
#include "unostorage.h"     /* disk authoring (brings blkdev.h): the disk verbs */
#include "unoauto_serial.h" /* 16550 UART backend: the NIC-independent transport */

#ifdef UNO_DEBUG

/* ---- freestanding libc + debug kernel symbols (no public header) --------- */
void        *memcpy(void *, const void *, unsigned long);
void        *memmove(void *, const void *, unsigned long);
unsigned long strlen(const char *);
void  uno_pc64_inject_key(int scan, int uni, int ctrl);
void  uno_pc64_inject_pointer(int x, int y, int btn);
int   pc64_shell_app_count(void);
int   pc64_shell_launch(int a);
void  pc64_shell_close_top(void);
void  uno_pc64_shutdown(void);
unsigned long long uno_dbg_uptime_ms(void);
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
/* raw-disk authoring (disks/arm/prepdisk verbs) - wraps unostorage + fat + fs */
void  uno_fat_remount(void);                                     /* fat.c   */
void  uno_fs_remap(void);                                        /* pc64_fs */
int   uno_fat_mkfs(uno_bdev *dev, unsigned long long first,
                   unsigned long long sectors, const char *label);

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

/* outbound byte queue (linear, compacted on flush) */
static char     g_tx[8192];
static int      g_txlen;
static unsigned g_tx_dropped;

/* inbound line assembly. 4 KB so a `put` frame can carry a big base64 chunk
 * (fewer synchronous round-trips = faster multi-MB pushes); still well inside
 * the 8 KB TCP rxq. */
static char     g_rx[4096];
static int      g_rxlen;

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
static UnoAutoProbeEnt g_pe[64];
static char            g_report[4096];

static void do_probe(const char *id)
{
    int n = unoauto_probe(g_pe, 64), i;
    for (i = 0; i < n; i++) {
        /* fields: kind state v1 v2 name - name LAST so it may contain spaces */
        char f[256]; SB b;
        sb_init(&b, f, sizeof f);
        sb_i(&b, g_pe[i].kind);  sb_c(&b, ' ');
        sb_i(&b, g_pe[i].state); sb_c(&b, ' ');
        sb_i(&b, (long)g_pe[i].v1); sb_c(&b, ' ');
        sb_i(&b, (long)g_pe[i].v2); sb_c(&b, ' ');
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

/* execute `verb args...` (id echoed on every RSP). args is the remainder. */
static void dispatch_cmd(const char *id, char *verb, char *args)
{
    if (!verb) { rsp(id, "err", "empty"); rsp(id, "end", 0); return; }

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
    if (!strcmp_(verb, "apps")) {
        char t[16]; SB b; sb_init(&b, t, sizeof t); sb_i(&b, pc64_shell_app_count()); t[b.len] = 0;
        rsp(id, "ok", t); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "launch")) {
        int ok = pc64_shell_launch((int)atol_(tok(&args)));
        rsp(id, ok ? "ok" : "err", ok ? "launched" : "no-app"); rsp(id, "end", 0); return;
    }
    if (!strcmp_(verb, "close")) {
        pc64_shell_close_top(); rsp(id, "ok", 0); rsp(id, "end", 0); return;
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
    if (!strcmp_(verb, "bootnext")) {
        int ok = uno_pc64_set_bootnext((unsigned)atol_(tok(&args)));
        rsp(id, ok ? "ok" : "err",
            ok ? "set" : "unavailable (detached / no runtime SetVariable)");
        rsp(id, "end", 0); return;
    }
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
     * discovery machinery is armed by the STRESS.CFG `discover` flag and pumped
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
    if (!strcmp_(verb, "test")) {
        do_test(id, tok(&args)); return;
    }
    if (!strcmp_(verb, "py")) {
        int rc = pc64_shell_py_exec(args ? args : "", g_report, (int)sizeof g_report);
        char *p = g_report;
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
    unoauto_log(UA_CH_SCRIPT, "remote: bad address '%s' in STRESS.CFG", v);
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
        } else if (ls == LINK_DEAD || g_tick > g_deadline) {
            g_tp->close();
            g_state = RS_DOWN; g_deadline = g_tick + 300;   /* retry ~5 s */
        }
        break;
    case RS_UP:
        drain_rx();
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
            g_state = RS_DOWN; g_deadline = g_tick + 300;
        } else if (g_pending_off && g_txlen == 0) {
            uno_pc64_shutdown();
        } else if (g_pending_reboot && g_txlen == 0) {
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

#endif /* UNO_DEBUG */
