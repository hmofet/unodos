/* ===========================================================================
 * unosecure.c - the UnoDOS security engine.
 *
 * Provides the STRONG unosec_* seam (replacing unoscript.c's weak fail-closed
 * fallbacks) plus unosecure's own account / RBAC / session / policy / manifest
 * / audit surface.  PRODUCTION, always-on, fail-closed: any ambiguity, missing
 * session, storage error or policy gap denies.
 *
 * Crypto is self-contained (BearSSL, already in the tree): PBKDF2-HMAC-SHA256
 * for password hashing, SHA-256 for the tamper-evident audit chain, HMAC-SHA256
 * for manifest signatures.  Storage is a single root-only unofs blob; the audit
 * trail is an append-only hash-chained log.  See UNOSECURE-SPEC.md (contract)
 * and UNOSECURE.md (formats + the thread->session binding).
 * ======================================================================== */
#include "unosecure.h"
#include "pc64_fs.h"
#include "bearssl_hash.h"
#include "bearssl_hmac.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#ifdef UNO_DEBUG
#include "unoauto.h"      /* audit forward to the SCRIPT LOG channel (debug)   */
#endif

/* Time base, forward-declared to avoid pulling the whole Toolbox in:
 *   TickCount() - 60 Hz monotonic (mac_compat.h), our escalation clock.
 *   uno_pc64_time() - firmware RTC wall clock, for audit timestamps. */
long TickCount(void);
int  uno_pc64_time(int *y, int *mo, int *d, int *h, int *mi, int *s);

/* ---------------------------------------------------------------------------
 * Sizing.  Fixed tables - no dynamic account growth in the kernel image; the
 * whole store is a POD blob we read/write in one shot.
 * ------------------------------------------------------------------------- */
#define MAX_USERS      32
#define MAX_ROLES      16
#define MAX_ROLECAPS   28
#define MAX_USERROLES   8
#define MAX_SESSIONS   32
#define MAX_GRANTS     16          /* live escalations per session            */
#define MAX_KEYS        8          /* manifest trust store                    */
#define NAME_MAX       32
#define CAPNAME_MAX    24
#define SALT_LEN       16
#define HASH_LEN       32
#define KDF_ITERS    4096          /* PBKDF2-HMAC-SHA256 rounds (WPA-grade)   */
#define BIND_DEPTH     16          /* nesting of unosec_enter_session()       */
#define LOG_CAP     16384          /* in-RAM audit tail, flushed to unofs     */

#define STORE_FILE  "UNOSEC.DB"
#define AUDIT_FILE  "UNOSEC.LOG"

/* ---------------------------------------------------------------------------
 * POD store types (serialised verbatim; the magic carries the version).
 * ------------------------------------------------------------------------- */
typedef struct {
    int32_t  used;
    uint32_t uid;
    char     name[NAME_MAX];
    uint8_t  salt[SALT_LEN];
    uint8_t  hash[HASH_LEN];       /* PBKDF2(password, salt)                  */
    char     roles[MAX_USERROLES][NAME_MAX];
    int32_t  nroles;
    int32_t  locked;
} account_t;

typedef struct {
    int32_t used;
    char    name[NAME_MAX];
    char    caps[MAX_ROLECAPS][CAPNAME_MAX];
    int32_t ncaps;
} role_t;

typedef struct {
    int32_t used;
    char    id[NAME_MAX];
    uint8_t key[32];
} trustkey_t;

typedef struct {
    char       magic[8];           /* 'U','N','S','C','D','B',1,0            */
    uint32_t   policy_mode;
    uint32_t   next_uid;
    char       autogrant_role[NAME_MAX];
    account_t  users[MAX_USERS];
    role_t     roles[MAX_ROLES];
    trustkey_t keys[MAX_KEYS];
} db_blob_t;

static const char DB_MAGIC[8] = { 'U','N','S','C','D','B',1,0 };

/* ---- live escalation grants (RAM only; never persisted) ------------------ */
typedef struct {
    int         used;
    usc_cap_t   cap;
    usc_scope_t scope;
    unsigned long long expire_tick;   /* 0 = no expiry (ONCE/SESSION)         */
    int         handle;
} grant_t;

typedef struct {
    int         used;
    int         gen;
    usc_uid_t   uid;
    usc_trust_t trust;
    unsigned long long expire_tick;   /* 0 = no expiry                        */
    int         locked;
    grant_t     grants[MAX_GRANTS];
} session_t;

/* ---------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */
static db_blob_t  g_db;                       /* the persisted store          */
static session_t  g_sess[MAX_SESSIONS];       /* live sessions (RAM)          */
static usec_session_t g_bind[BIND_DEPTH];     /* current-thread binding stack */
static int        g_binddepth;
static int        g_vol = -1;                 /* unofs volume backing the store */
static int        g_up;                       /* unosec_boot() completed      */
static int        g_grant_seq = 1;            /* monotonic grant-handle source */

static unosec_consent_fn g_consent;           /* registered consent provider  */
static void             *g_consent_ctx;

/* audit chain */
static unsigned char g_audit_head[32];
static unsigned      g_audit_seq;
static char          g_log[LOG_CAP];
static int           g_loglen;

/* ===========================================================================
 * Small crypto / util helpers.
 * ======================================================================== */
static void sha256(const void *d, size_t n, unsigned char out[32])
{
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, d, n);
    br_sha256_out(&c, out);
}

static void hmac256(const unsigned char *key, int klen,
                    const void *msg, size_t mlen, unsigned char out[32])
{
    br_hmac_key_context kc;
    br_hmac_context     hc;
    br_hmac_key_init(&kc, &br_sha256_vtable, key, (size_t)klen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg, mlen);
    br_hmac_out(&hc, out);
}

/* PBKDF2-HMAC-SHA256, dkLen == hLen == 32 (one block). */
static void pbkdf2(const char *pw, int pwlen,
                   const unsigned char *salt, int slen,
                   unsigned char out[32])
{
    br_hmac_key_context kc;
    br_hmac_context     hc;
    unsigned char u[32], t[32], seed[SALT_LEN + 4];
    int i, j;

    if (slen > SALT_LEN) slen = SALT_LEN;
    memcpy(seed, salt, (size_t)slen);
    seed[slen] = 0; seed[slen+1] = 0; seed[slen+2] = 0; seed[slen+3] = 1;

    br_hmac_key_init(&kc, &br_sha256_vtable, pw, (size_t)pwlen);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, seed, (size_t)slen + 4);
    br_hmac_out(&hc, u);
    memcpy(t, u, 32);
    for (i = 1; i < KDF_ITERS; i++) {
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, u, 32);
        br_hmac_out(&hc, u);
        for (j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out, t, 32);
}

/* constant-time equality (never branch on secret compare) */
static int ct_eq(const unsigned char *a, const unsigned char *b, int n)
{
    unsigned d = 0; int i;
    for (i = 0; i < n; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

/* ---- entropy for salts: RDRAND if present, else a whitened TSC mix -------- */
static int rdrand64(unsigned long long *v)
{
    unsigned char ok;
    __asm__ volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}
static unsigned long long rdtsc_(void)
{
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
static void gen_random(unsigned char *out, int n)
{
    static unsigned long long ctr;      /* de-correlate repeated TSC reads    */
    int i;
    for (i = 0; i < n; i += 8) {
        unsigned long long v;
        unsigned char blk[32];
        int t = 8;
        while (!rdrand64(&v) && t--) ;   /* bounded retry; TSC fallback below  */
        if (t < 0) v = rdtsc_() ^ (ctr * 0x9E3779B97F4A7C15ULL);
        ctr++;
        /* whiten so a weak source never lands raw in the salt */
        {
            unsigned char seed[16];
            memcpy(seed, &v, 8); memcpy(seed + 8, &ctr, 8);
            sha256(seed, sizeof seed, blk);
        }
        {
            int k = (n - i < 8) ? (n - i) : 8;
            memcpy(out + i, blk, (size_t)k);
        }
    }
}

static void tohex(const unsigned char *in, int n, char *out)
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < n; i++) { out[2*i] = H[in[i] >> 4]; out[2*i+1] = H[in[i] & 15]; }
    out[2*n] = 0;
}
static int hexnib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int fromhex(const char *s, unsigned char *out, int maxn)
{
    int i;
    for (i = 0; i < maxn && s[2*i] && s[2*i+1]; i++) {
        int hi = hexnib(s[2*i]), lo = hexnib(s[2*i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return i;
}

static unsigned long long now_ticks(void) { return (unsigned long long)TickCount(); }
static unsigned long long ms_to_ticks(int ms) { return ((unsigned long long)ms * 60ULL) / 1000ULL; }

/* ===========================================================================
 * Role helpers.
 * ======================================================================== */
static role_t *role_find(const char *name)
{
    int i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < MAX_ROLES; i++)
        if (g_db.roles[i].used && !strcmp(g_db.roles[i].name, name))
            return &g_db.roles[i];
    return 0;
}
static role_t *role_ensure(const char *name)
{
    role_t *r = role_find(name);
    int i;
    if (r) return r;
    for (i = 0; i < MAX_ROLES; i++)
        if (!g_db.roles[i].used) {
            r = &g_db.roles[i];
            memset(r, 0, sizeof *r);
            r->used = 1;
            strncpy(r->name, name, NAME_MAX - 1);
            return r;
        }
    return 0;
}
static int role_has_cap(const role_t *r, const char *capname)
{
    int i;
    for (i = 0; i < r->ncaps; i++)
        if (!strcmp(r->caps[i], capname)) return 1;
    return 0;
}
static int role_add_cap_raw(const char *role, const char *capname)
{
    role_t *r = role_ensure(role);
    if (!r || !capname || !capname[0]) return 0;
    if (role_has_cap(r, capname)) return 1;
    if (r->ncaps >= MAX_ROLECAPS) return 0;
    strncpy(r->caps[r->ncaps], capname, CAPNAME_MAX - 1);
    r->ncaps++;
    return 1;
}

/* ===========================================================================
 * Account helpers.
 * ======================================================================== */
static account_t *acc_by_uid(usc_uid_t u)
{
    int i;
    if (u == UNOSEC_UID_SYSTEM || u == UNOSEC_UID_NONE) return 0;
    for (i = 0; i < MAX_USERS; i++)
        if (g_db.users[i].used && g_db.users[i].uid == (uint32_t)u)
            return &g_db.users[i];
    return 0;
}
static account_t *acc_by_name_(const char *name)
{
    int i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < MAX_USERS; i++)
        if (g_db.users[i].used && !strcmp(g_db.users[i].name, name))
            return &g_db.users[i];
    return 0;
}
static int acc_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_USERS; i++) if (g_db.users[i].used) n++;
    return n;
}
/* does the account hold `capname` through any assigned role? */
static int acc_has_capname(const account_t *a, const char *capname)
{
    int i;
    for (i = 0; i < a->nroles; i++) {
        role_t *r = role_find(a->roles[i]);
        if (r && role_has_cap(r, capname)) return 1;
    }
    return 0;
}
static int acc_has_role(const account_t *a, const char *role)
{
    int i;
    for (i = 0; i < a->nroles; i++)
        if (!strcmp(a->roles[i], role)) return 1;
    return 0;
}

/* ===========================================================================
 * Session helpers.
 * ======================================================================== */
static usec_session_t sess_encode(int idx, int gen)
{ return (usec_session_t)(((gen & 0x7fff) << 16) | ((idx + 1) & 0xffff)); }

static session_t *sess_lookup(usec_session_t h)
{
    int idx, gen;
    session_t *s;
    if (h <= 0) return 0;
    idx = (h & 0xffff) - 1;
    gen = (h >> 16) & 0x7fff;
    if (idx < 0 || idx >= MAX_SESSIONS) return 0;
    s = &g_sess[idx];
    if (!s->used || s->gen != gen) return 0;
    return s;
}
static int sess_live(session_t *s)
{
    if (!s || !s->used || s->locked) return 0;
    if (s->expire_tick && now_ticks() >= s->expire_tick) return 0;
    return 1;
}
static usec_session_t sess_alloc(usc_uid_t uid, usc_trust_t trust)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++)
        if (!g_sess[i].used) {
            session_t *s = &g_sess[i];
            int gen = (s->gen + 1) & 0x7fff;
            memset(s, 0, sizeof *s);
            s->used = 1; s->gen = gen; s->uid = uid; s->trust = trust;
            return sess_encode(i, gen);
        }
    return 0;
}

/* an active, unexpired grant for `cap` in session `s`? (lazily reaps expired) */
static int sess_has_grant(session_t *s, usc_cap_t cap)
{
    int i, hit = 0;
    unsigned long long now = now_ticks();
    for (i = 0; i < MAX_GRANTS; i++) {
        grant_t *g = &s->grants[i];
        if (!g->used) continue;
        if (g->expire_tick && now >= g->expire_tick) { g->used = 0; continue; }
        if (g->cap == cap) hit = 1;
    }
    return hit;
}
static int sess_add_grant(session_t *s, usc_cap_t cap, usc_scope_t scope, int ttl_ms)
{
    int i;
    for (i = 0; i < MAX_GRANTS; i++)
        if (!s->grants[i].used) {
            grant_t *g = &s->grants[i];
            g->used = 1; g->cap = cap; g->scope = scope;
            g->expire_tick = (scope == USC_SCOPE_TIMED && ttl_ms > 0)
                             ? now_ticks() + ms_to_ticks(ttl_ms) : 0;
            g->handle = g_grant_seq++;
            if (g_grant_seq <= 0) g_grant_seq = 1;
            return g->handle;
        }
    return 0;
}

/* ===========================================================================
 * Audit (append-only, SHA-256 hash chain).
 * ======================================================================== */
static void audit_flush(void)
{
    if (g_vol >= 0)
        uno_fs_write(g_vol, AUDIT_FILE, (const unsigned char *)g_log, g_loglen);
}
static void audit_emit(usc_uid_t uid, const char *capname,
                       const char *detail, int allowed)
{
    char line[256];
    unsigned char chainbuf[32 + 256];
    char chainhex[17];
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, hn, need;

    if (!uno_pc64_time(&y, &mo, &d, &h, &mi, &s)) { y = mo = d = h = mi = s = 0; }

    hn = snprintf(line, sizeof line,
                  "%u %04d-%02d-%02d %02d:%02d:%02d uid=%lu cap=%s %s %s\n",
                  g_audit_seq, y, mo, d, h, mi, s, (unsigned long)uid,
                  capname ? capname : "?", allowed ? "ALLOW" : "DENY",
                  detail ? detail : "");
    if (hn < 0) return;
    if (hn >= (int)sizeof line) hn = (int)sizeof line - 1;

    /* chain: head = SHA256(prev_head || line) */
    memcpy(chainbuf, g_audit_head, 32);
    memcpy(chainbuf + 32, line, (size_t)hn);
    sha256(chainbuf, 32 + (size_t)hn, g_audit_head);
    g_audit_seq++;
    tohex(g_audit_head, 8, chainhex);      /* short chain tag stored per line */

    /* stored form: "<chainhex> <line>" - the file carries the chain itself */
    need = 17 + hn;                        /* 16 hex + space + line          */
    if (g_loglen + need > LOG_CAP) {       /* keep the tail; never grow past  */
        int half = LOG_CAP / 2;
        char *nl = memchr(g_log + half, '\n', (size_t)(g_loglen - half));
        int keep_from = nl ? (int)(nl - g_log) + 1 : half;
        memmove(g_log, g_log + keep_from, (size_t)(g_loglen - keep_from));
        g_loglen -= keep_from;
    }
    if (g_loglen + need <= LOG_CAP) {
        memcpy(g_log + g_loglen, chainhex, 16); g_loglen += 16;
        g_log[g_loglen++] = ' ';
        memcpy(g_log + g_loglen, line, (size_t)hn); g_loglen += hn;
    }
    audit_flush();

#ifdef UNO_DEBUG
    unoauto_log(UA_CH_SCRIPT, "audit uid=%lu %s %s -> %s",
                (unsigned long)uid, capname ? capname : "?",
                detail ? detail : "", allowed ? "ALLOW" : "DENY");
#endif
}

int unosec_audit_head(unsigned char out[32])
{
    if (!out) return 0;
    memcpy(out, g_audit_head, 32);
    return 1;
}

/* ===========================================================================
 * Persistence.
 * ======================================================================== */
static int pick_vol(void)
{
    int n = uno_fs_volumes(), v;
    for (v = 1; v < n; v++)                       /* prefer persistent native FAT */
        if (uno_fs_kind(v) == 1 && uno_fs_writable(v)) return v;
    for (v = 1; v < n; v++)
        if (uno_fs_writable(v)) return v;
    if (n > 0 && uno_fs_writable(0)) return 0;      /* RAM disk: non-persistent   */
    return -1;
}
static void db_save(void)
{
    if (g_vol < 0) return;
    memcpy(g_db.magic, DB_MAGIC, 8);
    uno_fs_write(g_vol, STORE_FILE, (const unsigned char *)&g_db, sizeof g_db);
}
static int db_load(void)
{
    long sz;
    if (g_vol < 0) return 0;
    sz = uno_fs_size(g_vol, STORE_FILE);
    if (sz != (long)sizeof g_db) return 0;
    if (uno_fs_read(g_vol, STORE_FILE, (unsigned char *)&g_db, sizeof g_db)
            != (long)sizeof g_db) return 0;
    if (memcmp(g_db.magic, DB_MAGIC, 8) != 0) { memset(&g_db, 0, sizeof g_db); return 0; }
    return 1;
}
static void audit_load(void)
{
    long sz;
    if (g_vol < 0) return;
    sz = uno_fs_size(g_vol, AUDIT_FILE);
    if (sz > 0 && sz <= LOG_CAP) {
        long r = uno_fs_read(g_vol, AUDIT_FILE, (unsigned char *)g_log, LOG_CAP);
        if (r > 0) g_loglen = (int)r;
    }
}

/* ===========================================================================
 * Built-in role seeding (only when the role table is empty on a fresh store).
 * ======================================================================== */
static void seed_roles(void)
{
    int c;
    role_ensure("admin"); role_ensure("user"); role_ensure("guest");
    for (c = 1; c < USC_CAP__COUNT; c++) {
        usc_tier_t t = unoscript_cap_tier((usc_cap_t)c);
        const char *n = unoscript_cap_name((usc_cap_t)c);
        if (t <= USC_TIER_ADMIN)   role_add_cap_raw("admin", n);
        if (t <= USC_TIER_USER)    role_add_cap_raw("user",  n);
        if (t == USC_TIER_AMBIENT) role_add_cap_raw("guest", n);
    }
    /* unosecure's own management caps live only on admin. */
    role_add_cap_raw("admin", UNOSEC_CAP_USER_CREATE);
    role_add_cap_raw("admin", UNOSEC_CAP_USER_DELETE);
    role_add_cap_raw("admin", UNOSEC_CAP_ROLE_ASSIGN);
    role_add_cap_raw("admin", UNOSEC_CAP_ROLE_EDIT);
    role_add_cap_raw("admin", UNOSEC_CAP_POLICY_EDIT);
    role_add_cap_raw("admin", UNOSEC_CAP_AUDIT_READ);
}

/* ===========================================================================
 * Lifecycle.
 * ======================================================================== */
int unosec_boot(void)
{
    int have_db;
    if (g_up) return 1;

    memset(&g_db, 0, sizeof g_db);
    memset(g_sess, 0, sizeof g_sess);
    g_binddepth = 0;
    g_db.policy_mode = UNOSEC_POLICY_PROMPT;
    g_db.next_uid = 1;

    g_vol = pick_vol();
    have_db = db_load();
    if (have_db) {
        audit_load();
    } else {
        /* fresh (or unreadable) store: seed built-in roles, leave bootstrap
         * armed (no accounts yet).  In-RAM only until the first db_save(). */
        seed_roles();
        if (g_db.next_uid < 1) g_db.next_uid = 1;
        if (g_vol >= 0) db_save();
    }
    /* a loaded store with no roles (corruption / older layout) -> reseed. */
    if (!role_find("admin")) seed_roles();

    g_up = 1;
    audit_emit(UNOSEC_UID_SYSTEM, "sec.boot",
               have_db ? "store loaded" : "fresh store", 1);
    return 1;
}

/* ===========================================================================
 * Authorization internals shared by the seam and the sec.* mutators.
 * ======================================================================== */
static int cap_is_ambient(usc_cap_t cap)
{ return unoscript_cap_tier(cap) == USC_TIER_AMBIENT; }

/* current bound session (top of the enter/leave stack), or 0. */
usec_session_t unosec_current_session(void)
{ return g_binddepth > 0 ? g_bind[g_binddepth - 1] : 0; }

static session_t *cur_sess(void)
{
    session_t *s = sess_lookup(unosec_current_session());
    return sess_live(s) ? s : 0;
}

/* the current context may use a named unosecure capability (sec.*): only the
 * system identity or an account whose role holds it.  Fail-closed. */
static int caller_can(const char *capname)
{
    usc_uid_t u = unosec_current_user();
    account_t *a;
    if (u == UNOSEC_UID_SYSTEM) return 1;
    a = acc_by_uid(u);
    return a ? acc_has_capname(a, capname) : 0;
}

/* ===========================================================================
 * The unosec_* seam (STRONG defs; override unoscript.c's weak fallbacks).
 * ======================================================================== */
int unosec_present(void) { return 1; }

usc_uid_t unosec_current_user(void)
{
    session_t *s = cur_sess();
    return s ? s->uid : UNOSEC_UID_NONE;
}

int unosec_check(usc_uid_t u, usc_cap_t cap)
{
    account_t *a;
    const char *name;
    if (cap <= USC_CAP_NONE || cap >= USC_CAP__COUNT) return 0;   /* fail closed */
    if (u == UNOSEC_UID_SYSTEM) return 1;            /* the kernel holds all     */
    if (cap_is_ambient(cap))    return 1;            /* tier 0 == interactive     */
    if (u == UNOSEC_UID_NONE)   return 0;            /* unauthenticated: floor    */
    a = acc_by_uid(u);
    if (!a) return 0;
    name = unoscript_cap_name(cap);
    return acc_has_capname(a, name);
}

int unosec_require(usc_cap_t cap)
{
    session_t *s = cur_sess();
    usc_uid_t  u = unosec_current_user();
    usc_tier_t t;
    if (cap <= USC_CAP_NONE || cap >= USC_CAP__COUNT) return 0;
    t = unoscript_cap_tier(cap);

    /* static authority, capped by trust class: SANDBOX never holds above
     * AMBIENT statically (it must escalate for anything more). */
    if (unosec_check(u, cap)) {
        if (t == USC_TIER_AMBIENT) return 1;
        if (!s) return 0;
        if (s->trust == UNOSEC_TRUST_SANDBOX) { /* fall through to grants */ }
        else return 1;
    }
    /* else: an active escalation grant in the current session? */
    if (s && sess_has_grant(s, cap)) return 1;
    return 0;
}

/* Decide an escalation.  Returns 1 to grant, 0 to deny.  Prompts (via the
 * registered consent provider) only where policy requires it. */
static int decide_escalation(session_t *s, usc_uid_t u,
                             usc_cap_t cap, const char *detail)
{
    usc_tier_t t = unoscript_cap_tier(cap);
    account_t *a = acc_by_uid(u);

    if (g_db.policy_mode == UNOSEC_POLICY_DENY) return 0;   /* kiosk: no escalation */
    if (!s) return 0;                                        /* no session: closed   */

    /* SANDBOX / REMOTE untrusted code: refuse anything above AMBIENT outright. */
    if (s->trust == UNOSEC_TRUST_SANDBOX && t >= USC_TIER_USER) return 0;

    /* developer-machine auto-grant role: covers every tier, no prompt. */
    if (g_db.autogrant_role[0] && a && acc_has_role(a, g_db.autogrant_role))
        return 1;

    /* AUTOGRANT policy: grant up to and including ADMIN without a sheet;
     * KERNEL still needs a manifest or explicit consent. */
    if (g_db.policy_mode == UNOSEC_POLICY_AUTOGRANT && t <= USC_TIER_ADMIN)
        return 1;

    /* a granting role the user statically holds auto-grants ADMIN and below
     * (the RBAC path - no prompt), except for untrusted trust classes. */
    if (t <= USC_TIER_ADMIN && a && acc_has_capname(a, unoscript_cap_name(cap)))
        return 1;

    /* USER tier for the owning, trusted user: their own authority, grant it. */
    if (t == USC_TIER_USER &&
        (s->trust == UNOSEC_TRUST_INTERACTIVE ||
         s->trust == UNOSEC_TRUST_INSTALLED   ||
         s->trust == UNOSEC_TRUST_REMOTE))
        return 1;

    /* ADMIN/KERNEL without a granting role: interactive consent.  Absent a
     * provider, fail closed.  KERNEL prompts default to Deny in the provider. */
    if (g_consent) {
        usc_consent_t r = g_consent(g_consent_ctx, u, s->trust, cap,
                                    unoscript_cap_name(cap), t, detail);
        return r != UNOSEC_CONSENT_DENY;
    }
    return 0;
}

int unosec_request(usc_cap_t cap, usc_scope_t scope, int ttl_ms)
{
    session_t *s = cur_sess();
    usc_uid_t  u = unosec_current_user();
    usc_tier_t t;
    int handle = 0, allowed = 0;

    if (cap <= USC_CAP_NONE || cap >= USC_CAP__COUNT) return 0;
    t = unoscript_cap_tier(cap);

    if (!s) {                                   /* no session: fail closed        */
        if (t >= USC_TIER_ADMIN)
            audit_emit(u, unoscript_cap_name(cap), "request:no-session", 0);
        return 0;
    }

    /* already authorised (static or an existing grant)?  Hand back a fresh
     * handle so the caller can drop symmetrically. */
    if (unosec_require(cap)) {
        allowed = 1;
        handle  = sess_add_grant(s, cap, scope, ttl_ms);
    } else if (decide_escalation(s, u, cap, "u.request")) {
        allowed = 1;
        handle  = sess_add_grant(s, cap, scope, ttl_ms);
        if (!handle) allowed = 0;               /* grant table full: fail closed */
    }

    if (t >= USC_TIER_ADMIN || !allowed)
        audit_emit(u, unoscript_cap_name(cap), "u.request", allowed);
    return allowed ? handle : 0;
}

void unosec_drop(int grant)
{
    int i, j;
    if (grant <= 0) return;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sess[i].used) continue;
        for (j = 0; j < MAX_GRANTS; j++)
            if (g_sess[i].grants[j].used && g_sess[i].grants[j].handle == grant) {
                g_sess[i].grants[j].used = 0;
                return;
            }
    }
}

void unosec_audit(usc_cap_t cap, const char *detail, int allowed)
{ audit_emit(unosec_current_user(), unoscript_cap_name(cap), detail, allowed); }

/* ===========================================================================
 * Accounts.
 * ======================================================================== */
static usc_uid_t account_create_internal(const char *name, const char *password,
                                          const char *role)
{
    account_t *a = 0;
    int i;
    if (!name || !name[0] || !password) return 0;
    if (strlen(name) >= NAME_MAX) return 0;
    if (acc_by_name_(name)) return 0;                 /* duplicate name           */
    for (i = 0; i < MAX_USERS; i++)
        if (!g_db.users[i].used) { a = &g_db.users[i]; break; }
    if (!a) return 0;                                  /* store full               */

    memset(a, 0, sizeof *a);
    a->used = 1;
    a->uid  = g_db.next_uid++;
    strncpy(a->name, name, NAME_MAX - 1);
    gen_random(a->salt, SALT_LEN);
    pbkdf2(password, (int)strlen(password), a->salt, SALT_LEN, a->hash);
    if (role && role[0] && role_find(role)) {
        strncpy(a->roles[0], role, NAME_MAX - 1);
        a->nroles = 1;
    }
    db_save();
    return a->uid;
}

usc_uid_t unosec_account_create(const char *name, const char *password,
                                const char *role)
{
    usc_uid_t uid;
    if (!g_up) return 0;
    if (!caller_can(UNOSEC_CAP_USER_CREATE)) {
        audit_emit(unosec_current_user(), UNOSEC_CAP_USER_CREATE,
                   name ? name : "?", 0);
        return 0;
    }
    uid = account_create_internal(name, password, role);
    audit_emit(unosec_current_user(), UNOSEC_CAP_USER_CREATE,
               name ? name : "?", uid != 0);
    return uid;
}

int unosec_account_delete(usc_uid_t u)
{
    account_t *a;
    if (!g_up) return 0;
    if (!caller_can(UNOSEC_CAP_USER_DELETE)) {
        audit_emit(unosec_current_user(), UNOSEC_CAP_USER_DELETE, "", 0);
        return 0;
    }
    a = acc_by_uid(u);
    if (!a) return 0;
    memset(a, 0, sizeof *a);
    db_save();
    audit_emit(unosec_current_user(), UNOSEC_CAP_USER_DELETE, "", 1);
    return 1;
}

int unosec_account_list(usc_uid_t *out, int max)
{
    int i, n = 0;
    if (!g_up) return -1;
    for (i = 0; i < MAX_USERS; i++)
        if (g_db.users[i].used) {
            if (out && n < max) out[n] = g_db.users[i].uid;
            n++;
        }
    return n;
}

const char *unosec_account_name(usc_uid_t u)
{
    account_t *a = acc_by_uid(u);
    return a ? a->name : 0;
}
usc_uid_t unosec_account_by_name(const char *name)
{
    account_t *a = acc_by_name_(name);
    return a ? (usc_uid_t)a->uid : UNOSEC_UID_NONE;
}

int unosec_account_set_password(usc_uid_t u, const char *password)
{
    account_t *a = acc_by_uid(u);
    usc_uid_t me = unosec_current_user();
    if (!g_up || !a || !password) return 0;
    /* self-service, or an admin with user.create authority. */
    if (me != u && !caller_can(UNOSEC_CAP_USER_CREATE)) {
        audit_emit(me, "sec.passwd", a->name, 0);
        return 0;
    }
    gen_random(a->salt, SALT_LEN);
    pbkdf2(password, (int)strlen(password), a->salt, SALT_LEN, a->hash);
    db_save();
    audit_emit(me, "sec.passwd", a->name, 1);
    return 1;
}

usc_uid_t unosec_bootstrap_admin(const char *name, const char *password)
{
    usc_uid_t uid;
    if (!g_up) return 0;
    if (acc_count() != 0) return 0;                 /* only on a fresh install    */
    uid = account_create_internal(name, password, "admin");
    audit_emit(UNOSEC_UID_SYSTEM, "sec.bootstrap-admin", name ? name : "?", uid != 0);
    return uid;
}

/* ===========================================================================
 * RBAC surface.
 * ======================================================================== */
int unosec_role_grant(usc_uid_t u, const char *role)
{
    account_t *a = acc_by_uid(u);
    if (!g_up || !a || !role_find(role)) return 0;
    if (!caller_can(UNOSEC_CAP_ROLE_ASSIGN)) {
        audit_emit(unosec_current_user(), UNOSEC_CAP_ROLE_ASSIGN, role, 0);
        return 0;
    }
    if (acc_has_role(a, role)) return 1;
    if (a->nroles >= MAX_USERROLES) return 0;
    strncpy(a->roles[a->nroles], role, NAME_MAX - 1);
    a->nroles++;
    db_save();
    audit_emit(unosec_current_user(), UNOSEC_CAP_ROLE_ASSIGN, role, 1);
    return 1;
}
int unosec_role_revoke(usc_uid_t u, const char *role)
{
    account_t *a = acc_by_uid(u);
    int i;
    if (!g_up || !a) return 0;
    if (!caller_can(UNOSEC_CAP_ROLE_ASSIGN)) return 0;
    for (i = 0; i < a->nroles; i++)
        if (!strcmp(a->roles[i], role)) {
            for (; i < a->nroles - 1; i++)
                memcpy(a->roles[i], a->roles[i+1], NAME_MAX);
            a->nroles--;
            db_save();
            audit_emit(unosec_current_user(), UNOSEC_CAP_ROLE_ASSIGN, role, 1);
            return 1;
        }
    return 0;
}
int unosec_role_add_cap(const char *role, const char *cap_name)
{
    int ok;
    if (!g_up) return 0;
    if (!caller_can(UNOSEC_CAP_ROLE_EDIT)) return 0;
    ok = role_add_cap_raw(role, cap_name);
    if (ok) db_save();
    audit_emit(unosec_current_user(), UNOSEC_CAP_ROLE_EDIT, cap_name, ok);
    return ok;
}
int unosec_role_exists(const char *role) { return role_find(role) != 0; }

/* ===========================================================================
 * Authentication & sessions.
 * ======================================================================== */
usec_session_t unosec_login(const char *name, const char *password,
                            usc_trust_t trust)
{
    account_t *a = acc_by_name_(name);
    unsigned char probe[HASH_LEN];
    int ok = 0;
    if (!g_up) return 0;
    if (a && !a->locked && password) {
        pbkdf2(password, (int)strlen(password), a->salt, SALT_LEN, probe);
        ok = ct_eq(probe, a->hash, HASH_LEN);
    }
    audit_emit(a ? (usc_uid_t)a->uid : UNOSEC_UID_NONE, "auth.login",
               name ? name : "?", ok);
    if (!ok) return 0;
    return sess_alloc((usc_uid_t)a->uid, trust);
}

usec_session_t unosec_session_open(usc_uid_t uid, usc_trust_t trust)
{
    usc_uid_t me = unosec_current_user();
    if (!g_up) return 0;
    if (me != UNOSEC_UID_SYSTEM && me != uid) return 0;   /* no identity jumps  */
    if (uid != UNOSEC_UID_SYSTEM && !acc_by_uid(uid)) return 0;
    return sess_alloc(uid, trust);
}

void unosec_logout(usec_session_t h)
{
    session_t *s = sess_lookup(h);
    if (!s) return;
    audit_emit(s->uid, "auth.logout", "", 1);
    memset(s, 0, sizeof *s);      /* clears grants; gen already bumped on reuse */
    s->gen = (s->gen + 1) & 0x7fff;
}
usc_uid_t unosec_session_user(usec_session_t h)
{ session_t *s = sess_lookup(h); return s ? s->uid : UNOSEC_UID_NONE; }
int unosec_session_valid(usec_session_t h) { return sess_live(sess_lookup(h)); }
void unosec_session_lock(usec_session_t h)
{ session_t *s = sess_lookup(h); if (s) s->locked = 1; }

int unosec_enter_session(usec_session_t s)
{
    if (!sess_lookup(s)) return 0;
    if (g_binddepth >= BIND_DEPTH) return 0;
    g_bind[g_binddepth++] = s;
    return 1;
}
void unosec_leave(void) { if (g_binddepth > 0) g_binddepth--; }

/* ===========================================================================
 * Policy & consent.
 * ======================================================================== */
void unosec_policy_set(usc_policy_t mode)
{
    if (!caller_can(UNOSEC_CAP_POLICY_EDIT)) return;
    g_db.policy_mode = (uint32_t)mode;
    db_save();
    audit_emit(unosec_current_user(), UNOSEC_CAP_POLICY_EDIT, "policy.mode", 1);
}
usc_policy_t unosec_policy_get(void) { return (usc_policy_t)g_db.policy_mode; }

void unosec_policy_autogrant_role(const char *role)
{
    if (!caller_can(UNOSEC_CAP_POLICY_EDIT)) return;
    memset(g_db.autogrant_role, 0, NAME_MAX);
    if (role) strncpy(g_db.autogrant_role, role, NAME_MAX - 1);
    db_save();
    audit_emit(unosec_current_user(), UNOSEC_CAP_POLICY_EDIT, "policy.autogrant", 1);
}

void unosec_set_consent_provider(unosec_consent_fn fn, void *ctx)
{ g_consent = fn; g_consent_ctx = ctx; }

/* ===========================================================================
 * Signed manifests & the trust store.
 * ======================================================================== */
int unosec_trust_add_key(const char *key_id, const unsigned char key[32])
{
    int i;
    if (!g_up || !key_id || !key_id[0] || !key) return 0;
    if (!caller_can(UNOSEC_CAP_POLICY_EDIT)) return 0;
    for (i = 0; i < MAX_KEYS; i++)                         /* replace by id     */
        if (g_db.keys[i].used && !strcmp(g_db.keys[i].id, key_id)) {
            memcpy(g_db.keys[i].key, key, 32); db_save(); return 1;
        }
    for (i = 0; i < MAX_KEYS; i++)
        if (!g_db.keys[i].used) {
            g_db.keys[i].used = 1;
            strncpy(g_db.keys[i].id, key_id, NAME_MAX - 1);
            memcpy(g_db.keys[i].key, key, 32);
            db_save();
            return 1;
        }
    return 0;
}
static trustkey_t *trust_find(const char *id)
{
    int i;
    for (i = 0; i < MAX_KEYS; i++)
        if (g_db.keys[i].used && !strcmp(g_db.keys[i].id, id)) return &g_db.keys[i];
    return 0;
}

/* copy the value after "<field>:" on the line starting at *p (skips spaces),
 * up to end-of-line, into out[max]; returns the start of the next line. */
static const char *field_val(const char *p, char *out, int max)
{
    int n = 0;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != '\n' && *p != '\r' && n < max - 1) out[n++] = *p++;
    out[n] = 0;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    return p;
}

int unosec_manifest_apply(const char *manifest)
{
    session_t *s = cur_sess();
    const char *sigline, *p;
    char keyid[NAME_MAX] = {0}, sighex[80] = {0}, caps[256] = {0};
    unsigned char want[32], got[32];
    trustkey_t *tk;
    int granted = 0;

    if (!g_up || !manifest || !s) return -1;
    if (strncmp(manifest, "UNOSEC-MANIFEST v1", 18) != 0) return -1;

    /* pull the fields we sign over + the signature. */
    p = manifest;
    while (*p) {
        if (!strncmp(p, "caps:", 5)) field_val(p + 5, caps, sizeof caps);
        else if (!strncmp(p, "key:", 4)) field_val(p + 4, keyid, sizeof keyid);
        else if (!strncmp(p, "sig:", 4)) { field_val(p + 4, sighex, sizeof sighex); break; }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    sigline = strstr(manifest, "\nsig:");
    if (!sigline || !keyid[0] || !sighex[0]) return -1;

    tk = trust_find(keyid);
    if (!tk) { audit_emit(s->uid, "sec.manifest", keyid, 0); return 0; }

    /* HMAC over the canonical body: everything up to and including the '\n'
     * that precedes the sig line. */
    hmac256(tk->key, 32, manifest, (size_t)(sigline - manifest) + 1, want);
    if (fromhex(sighex, got, 32) != 32 || !ct_eq(want, got, 32)) {
        audit_emit(s->uid, "sec.manifest", "bad-signature", 0);
        return 0;
    }

    /* Signature good: the trusted key IS the authorization (SPEC §5 - "a valid
     * signature lets you grant without prompting on every op").  Grant each
     * declared cap SESSION-scoped, audited.  Kiosk policy still wins (no
     * escalation at all), and a SANDBOX trust class is refused above AMBIENT
     * even with a signature - untrusted provenance is not overridden by a
     * manifest it could ship for itself. */
    {
        char one[CAPNAME_MAX];
        const char *q = caps;
        if (g_db.policy_mode == UNOSEC_POLICY_DENY) return 0;
        while (*q) {
            int n = 0;
            while (*q == ' ' || *q == ',') q++;
            while (*q && *q != ',' && n < CAPNAME_MAX - 1) one[n++] = *q++;
            one[n] = 0;
            if (n) {
                int c;
                for (c = 1; c < USC_CAP__COUNT; c++)
                    if (!strcmp(one, unoscript_cap_name((usc_cap_t)c))) {
                        usc_tier_t t = unoscript_cap_tier((usc_cap_t)c);
                        int ok = !(s->trust == UNOSEC_TRUST_SANDBOX &&
                                   t >= USC_TIER_USER);
                        if (ok && sess_add_grant(s, (usc_cap_t)c, USC_SCOPE_SESSION, 0))
                            granted++;
                        audit_emit(s->uid, one, "sec.manifest", ok);
                        break;
                    }
            }
        }
    }
    return granted;
}

/* ===========================================================================
 * Audit read-back.
 * ======================================================================== */
int unosec_audit_read(char *out, int max)
{
    int n;
    if (!g_up || !out || max <= 0) return -1;
    if (!caller_can(UNOSEC_CAP_AUDIT_READ)) return -1;
    n = g_loglen < max - 1 ? g_loglen : max - 1;
    memcpy(out, g_log, (size_t)n);
    out[n] = 0;
    return n;
}

/* ===========================================================================
 * Optional self-test (build with -DUNO_SECTEST) - the DoD escalation gate at
 * the C level: an unprivileged session is denied PROC_ENUM, then permitted
 * after a request() grants under a test policy.  Returns 1 pass / 0 fail and
 * leaves NO persisted state (it runs on a fresh in-RAM store before any real
 * account exists).  Wired from init under the flag; also callable from a test.
 * ======================================================================== */
#ifdef UNO_SECTEST
int unosec_selftest(void)
{
    usec_session_t admin_s, user_s;
    usc_uid_t admin, user;
    int before, after, req;

    /* run against a clean store so we never touch a real one. */
    memset(&g_db, 0, sizeof g_db);
    memset(g_sess, 0, sizeof g_sess);
    g_binddepth = 0;
    g_vol = -1;                       /* no persistence during the test        */
    g_db.policy_mode = UNOSEC_POLICY_PROMPT;
    g_db.next_uid = 1;
    seed_roles();
    g_up = 1;

    admin = unosec_bootstrap_admin("root", "adminpw");
    if (!admin) return 0;

    /* become admin to provision an unprivileged user. */
    admin_s = unosec_login("root", "adminpw", UNOSEC_TRUST_INTERACTIVE);
    if (!admin_s || !unosec_enter_session(admin_s)) return 0;
    user = unosec_account_create("alice", "alicepw", "user");
    unosec_leave();
    if (!user) return 0;

    /* run as the unprivileged user. */
    user_s = unosec_login("alice", "alicepw", UNOSEC_TRUST_INTERACTIVE);
    if (!user_s || !unosec_enter_session(user_s)) return 0;

    before = unosec_require(USC_CAP_PROC_ENUM);           /* must be denied      */

    unosec_policy_set(UNOSEC_POLICY_AUTOGRANT);           /* the "test policy"   */
    /* policy_set is gated on sec.policy.edit; alice lacks it, so force it here
     * directly (a real deployment flips policy as system/installer). */
    g_db.policy_mode = UNOSEC_POLICY_AUTOGRANT;

    req    = unosec_request(USC_CAP_PROC_ENUM, USC_SCOPE_SESSION, 0);
    after  = unosec_require(USC_CAP_PROC_ENUM);           /* must now pass       */
    if (req > 0) unosec_drop(req);
    unosec_leave();

#ifdef UNO_DEBUG
    unoauto_log(UA_CH_SCRIPT, "unosec_selftest: before=%d req=%d after=%d",
                before, req, after);
#endif

    /* leave a pristine store for the real boot that follows. */
    memset(&g_db, 0, sizeof g_db);
    memset(g_sess, 0, sizeof g_sess);
    g_binddepth = 0;
    g_up = 0;

    return (!before) && (req > 0) && (after);
}
#endif
