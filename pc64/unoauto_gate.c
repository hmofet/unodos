/* ===========================================================================
 * UnoDOS/pc64 - the unoautomate privilege gate.  Contract + the whole design
 * rationale: unoauto_gate.h.  Read that first.
 *
 * This file is deliberately the ONLY place that knows the answer to "may this
 * remote command run?".  unoauto_remote.c asks; it does not decide.  Keeping
 * the policy in one small file is what makes it reviewable: the verb table
 * below is the complete list of what a URC link can reach, and anything not in
 * it is refused.
 * ======================================================================== */
#include "unoauto_gate.h"
#include "unoauto.h"
#include "unosecure.h"
#include "tls_entropy.h"

/* freestanding libc + the 60 Hz monotonic clock (see unosecure.c's note) */
void *memset(void *, int, unsigned long);
long  TickCount(void);
/* DEBUG.CFG reader - defined for production in unoauto_compat.c, so the
 * `urc-auth` opt-in below costs a production build nothing. */
int   pc64_stress_cfg_flag(const char *key);
/* set while the security UI is modal (uefi_main.c / pc64_accounts.c) */
int   uno_pc64_input_locked(void);

/* ---- state ---------------------------------------------------------------
 * All of it is boot-lifetime only: nothing here is persisted.  A token that
 * survived a reboot would be a standing credential nobody remembers granting. */
static int          g_armed;
static unsigned     g_powers;                        /* UNOAUTO_P_* mask       */
static char         g_token[UNOAUTO_TOKEN_BUF];
static usc_uid_t    g_owner = UNOSEC_UID_NONE;
static char         g_owner_name[32];
static usec_session_t g_console_sess;                /* the arming session     */
static usec_session_t g_link_sess;                   /* TRUST_REMOTE, the link */
static int          g_authed;                        /* current link is auth'd */
static int          g_badauth;                       /* failed auth attempts   */
static int          g_lockout;                       /* disarm on the next tick*/
static int          g_grants[3];                     /* handles, to drop later */

#define BADAUTH_MAX 3          /* then disarm: no brute-force oracle on a LAN */

/* ---- tiny local string helpers (this file predates no libc guarantee) ----- */
static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void strcpy_n(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ===========================================================================
 * The verb table - the complete reachable surface of a URC link.
 *
 * One row per verb.  The split follows the blast radius, not the subsystem:
 *
 *   OBSERVE  reads.  Machine state, log stream, screen contents, what disks
 *            and devices exist.  ADMIN tier: this sees the whole machine,
 *            including whatever the user has on screen.
 *   DRIVE    steers the running system: synthetic input, app launch/close,
 *            running a registered test suite, the dead-man's switch.  Bounded
 *            by what a person at the keyboard could do.
 *   SYSTEM   the destructive tail: raw sector writes, GPT/FAT authoring, file
 *            upload, install, arbitrary Python, NIC register pokes, power and
 *            boot-order control.  KERNEL tier - this is where a link can lose
 *            you the machine, so it is a separate grant the console user has
 *            to consent to on its own.
 *
 * Judgement calls worth recording, since a later reader will second-guess them:
 *   - `readsec` is SYSTEM, not OBSERVE.  It is read-only, but it reads RAW
 *     SECTORS: every file on the disk, past any filesystem permission.  That
 *     is disclosure of the whole machine, not observation of it.
 *   - `iwl`/`eth` are SYSTEM.  They look like diagnostics and mostly are, but
 *     both subcommand sets can WRITE device registers (csw/wreg).
 *   - `screen` is OBSERVE.  It is the read half of remote desktop and pairs
 *     with key/pointer under DRIVE.
 *   - `test` is DRIVE.  Its suites are debug-only anyway (weak-stubbed away in
 *     production), so in a shipped image it answers "unavailable".
 * ======================================================================== */
typedef struct { const char *verb; unsigned power; } GateRow;

static const GateRow GATE[] = {
    /* -- ungated: the pre-auth handshake -- */
    { "auth",     0 },
    { "help",     0 },

    /* -- OBSERVE -- */
    { "probe",    UNOAUTO_P_OBSERVE },
    { "log",      UNOAUTO_P_OBSERVE },
    { "uptime",   UNOAUTO_P_OBSERVE },
    { "apps",     UNOAUTO_P_OBSERVE },
    { "vols",     UNOAUTO_P_OBSERVE },
    { "disks",    UNOAUTO_P_OBSERVE },
    { "devices",  UNOAUTO_P_OBSERVE },
    { "screen",   UNOAUTO_P_OBSERVE },
#ifdef UNO_DEBUG
    { "disc",     UNOAUTO_P_OBSERVE },   /* self-test verb: debug builds only    */
#endif
    { "caps",     UNOAUTO_P_OBSERVE },
    /* unostream: exports what the screen shows (plus nothing else), so it is
     * OBSERVE exactly like `screen` - see the judgement notes above. */
    { "stream",   UNOAUTO_P_OBSERVE },

    /* -- DRIVE -- */
    { "key",      UNOAUTO_P_DRIVE },
    { "pointer",  UNOAUTO_P_DRIVE },
    { "launch",   UNOAUTO_P_DRIVE },
    /* rescan re-reads APPS\ and can change the app set the shell offers, so it
     * is DRIVE rather than OBSERVE: it changes the machine, not just the view.
     * The table is fail-closed, so this row lands in the same commit as the
     * verb - a verb without a row is refused, never ambient. */
    { "rescan",   UNOAUTO_P_DRIVE },
    { "close",    UNOAUTO_P_DRIVE },
    { "test",     UNOAUTO_P_DRIVE },
    { "guard",    UNOAUTO_P_DRIVE },
    { "pet",      UNOAUTO_P_DRIVE },
    { "safe",     UNOAUTO_P_DRIVE },

    /* -- SYSTEM -- */
    { "put",      UNOAUTO_P_SYSTEM },
    { "mkdir",    UNOAUTO_P_SYSTEM },
    { "install",  UNOAUTO_P_SYSTEM },
    { "arm",      UNOAUTO_P_SYSTEM },
    { "disarm",   UNOAUTO_P_SYSTEM },
    { "readsec",  UNOAUTO_P_SYSTEM },
    { "writesec", UNOAUTO_P_SYSTEM },
    { "gptinit",  UNOAUTO_P_SYSTEM },
    { "mkpart",   UNOAUTO_P_SYSTEM },
    { "mkfs",     UNOAUTO_P_SYSTEM },
    { "prepdisk", UNOAUTO_P_SYSTEM },
    { "makeboot", UNOAUTO_P_SYSTEM },
    { "bootnext", UNOAUTO_P_SYSTEM },
    { "reboot",   UNOAUTO_P_SYSTEM },
    { "poweroff", UNOAUTO_P_SYSTEM },
    { "py",       UNOAUTO_P_SYSTEM },
    { "iwl",      UNOAUTO_P_SYSTEM },
    { "eth",      UNOAUTO_P_SYSTEM },
    { "hwwdt",    UNOAUTO_P_SYSTEM },
#ifdef UNO_DEBUG
    { "nst",      UNOAUTO_P_SYSTEM },    /* netsock self-test: debug builds only */
#endif
};
#define GATE_N ((int)(sizeof GATE / sizeof GATE[0]))

static const GateRow *gate_find(const char *verb)
{
    int i;
    for (i = 0; i < GATE_N; i++)
        if (streq(GATE[i].verb, verb)) return &GATE[i];
    return 0;                       /* unknown verb -> caller refuses */
}

static usc_cap_t power_cap(unsigned p)
{
    if (p & UNOAUTO_P_SYSTEM)  return USC_CAP_AUTOMATE_SYSTEM;
    if (p & UNOAUTO_P_DRIVE)   return USC_CAP_AUTOMATE_DRIVE;
    if (p & UNOAUTO_P_OBSERVE) return USC_CAP_AUTOMATE_OBSERVE;
    return USC_CAP_NONE;
}

usc_cap_t unoauto_gate_verb_cap(const char *verb)
{
    const GateRow *r = gate_find(verb);
    return r ? power_cap(r->power) : USC_CAP_AUTOMATE_SYSTEM;   /* fail closed */
}

/* ===========================================================================
 * Arming.
 * ======================================================================== */

/* Mint the connection PIN. */
static int mint_token(void)
{
    /* Six decimal digits (see the note in unoauto_gate.h for why six is
     * enough HERE).  Drawn by REJECTION, not `byte % 10`: 256 is not a multiple
     * of ten, so the modulo would make 0-5 about 20% likelier than 6-9 and hand
     * an attacker a better-than-uniform first guess.  Bytes >= 250 are thrown
     * away instead, which costs a couple of extra draws and nothing else.
     *
     * A generous over-draw up front, then top-ups: tls_entropy_get is the
     * production source and FAILS CLOSED (tls_entropy.h) - on a box where
     * neither RDRAND nor conditioned jitter qualifies it returns 0 and we
     * refuse to arm rather than mint a guessable PIN from a TSC counter.  A
     * machine that cannot make a credential should say so, not make a weak one
     * - the same argument tls_entropy.c makes for keys. */
    unsigned char raw[UNOAUTO_TOKEN_CHARS * 4];
    int i = 0, n = 0, refills = 0;
    if (!tls_entropy_get(raw, (int)sizeof raw)) { g_token[0] = 0; return 0; }
    while (n < UNOAUTO_TOKEN_CHARS) {
        if (i >= (int)sizeof raw) {                  /* astronomically unlikely */
            if (++refills > 4 || !tls_entropy_get(raw, (int)sizeof raw)) {
                g_token[0] = 0; memset(raw, 0, sizeof raw); return 0;
            }
            i = 0;
        }
        { unsigned char b = raw[i++];
          if (b < 250) g_token[n++] = (char)('0' + (b % 10)); }
    }
    g_token[UNOAUTO_TOKEN_CHARS] = 0;
    memset(raw, 0, sizeof raw);
    return 1;
}

unsigned unoauto_gate_arm(unsigned want, const char *how)
{
    static const struct { unsigned bit; usc_cap_t cap; int slot; } POWERS[3] = {
        { UNOAUTO_P_OBSERVE, USC_CAP_AUTOMATE_OBSERVE, 0 },
        { UNOAUTO_P_DRIVE,   USC_CAP_AUTOMATE_DRIVE,   1 },
        { UNOAUTO_P_SYSTEM,  USC_CAP_AUTOMATE_SYSTEM,  2 },
    };
    usec_session_t cs = unosec_current_session();
    unsigned got = 0;
    int i;

    if (!want) return 0;
    /* No bound session = nobody has signed in.  Arming a remote-control channel
     * as "whoever happens to be standing here" is exactly the thing this gate
     * exists to prevent, so fail closed and let the UI say "sign in first". */
    if (!cs || !unosec_session_valid(cs)) {
        unosec_audit(USC_CAP_AUTOMATE_OBSERVE, "urc:arm no-session", 0);
        return 0;
    }
    /* Re-arming: keep what is already held, escalate only what is new.  The
     * token survives, so a user adding SYSTEM to a live link does not have to
     * re-type a new token on the dev PC. */
    if (g_armed && unosec_current_user() != g_owner) {
        unosec_audit(USC_CAP_AUTOMATE_OBSERVE, "urc:arm owner-mismatch", 0);
        return 0;
    }

    for (i = 0; i < 3; i++) {
        int h;
        if (!(want & POWERS[i].bit))   continue;
        if (g_powers & POWERS[i].bit) { got |= POWERS[i].bit; continue; }
        /* SESSION scope: the grant lives as long as the console session does,
         * which is precisely how long the token should work.  This is the call
         * that draws the consent sheet when the user's roles do not already
         * cover the capability. */
        h = unosec_request(POWERS[i].cap, USC_SCOPE_SESSION, 0);
        if (h > 0) { got |= POWERS[i].bit; g_grants[POWERS[i].slot] = h; }
    }

    got |= g_powers;
    if (!got) {                                  /* refused everything */
        unosec_audit(USC_CAP_AUTOMATE_OBSERVE, "urc:arm denied", 0);
        return 0;
    }

    if (!g_armed) {
        if (!mint_token()) {                     /* no defensible entropy */
            unosec_audit(USC_CAP_AUTOMATE_OBSERVE, "urc:arm no-entropy", 0);
            return 0;
        }
        /* The link's own identity: same user, REMOTE trust class.  Opened here,
         * while the console session is bound and proven, so the remote path
         * never has to establish identity for itself. */
        g_link_sess = unosec_session_open(unosec_current_user(),
                                          UNOSEC_TRUST_REMOTE);
        g_owner      = unosec_current_user();
        g_console_sess = cs;
        strcpy_n(g_owner_name, unosec_account_name(g_owner), (int)sizeof g_owner_name);
        g_armed = 1;
        g_badauth = 0;
    }
    g_powers = got;
    g_authed = 0;                                /* a fresh arm re-authenticates */

    /* Bring the channel up now that it is allowed to exist.  A no-op if it is
     * already running (re-arming to add a power keeps the live link). */
    { void unoauto_remote_boot(void); unoauto_remote_boot(); }

    unoauto_log(UA_CH_SCRIPT, "urc: armed by %s via %s (observe=%d drive=%d system=%d)",
                g_owner_name[0] ? g_owner_name : "?", how ? how : "?",
                (got & UNOAUTO_P_OBSERVE) != 0, (got & UNOAUTO_P_DRIVE) != 0,
                (got & UNOAUTO_P_SYSTEM) != 0);
    unosec_audit(power_cap(got), "urc:arm", 1);
    return got;
}

void unoauto_gate_disarm(const char *why)
{
    int i;
    void unoauto_remote_stop(void);              /* unoauto_remote.c */
    if (!g_armed) return;
    unoauto_remote_stop();
    for (i = 0; i < 3; i++) { if (g_grants[i] > 0) unosec_drop(g_grants[i]); g_grants[i] = 0; }
    if (g_link_sess) unosec_logout(g_link_sess);
    g_link_sess = 0; g_console_sess = 0;
    g_armed = 0; g_powers = 0; g_authed = 0; g_badauth = 0; g_lockout = 0;
    memset(g_token, 0, sizeof g_token);
    unoauto_log(UA_CH_SCRIPT, "urc: disarmed (%s)", why ? why : "?");
    unosec_audit(USC_CAP_AUTOMATE_OBSERVE, "urc:disarm", 1);
    g_owner = UNOSEC_UID_NONE; g_owner_name[0] = 0;
}

int         unoauto_gate_armed(void)      { return g_armed; }
unsigned    unoauto_gate_powers(void)     { return g_powers; }
const char *unoauto_gate_token(void)      { return g_armed ? g_token : ""; }
usc_uid_t   unoauto_gate_owner(void)      { return g_owner; }
const char *unoauto_gate_owner_name(void) { return g_armed ? g_owner_name : ""; }
usec_session_t unoauto_gate_link_session(void) { return g_armed ? g_link_sess : 0; }

void unoauto_gate_tick(void)
{
    /* A token must never outlive the login that made it.  The console session
     * ending - sign out, lock, expiry - stands the channel down on the next
     * frame, without the user having to remember to disarm.
     *
     * g_console_sess == 0 means the arm did NOT come from a console user: the
     * only way that happens is the debug `urc-auth=<token>` hook, which has no
     * session to outlive.  Checking it here rather than assuming matters - the
     * first version of this function disarmed the debug arm on the very next
     * frame, so the listener came up and vanished before any client could dial
     * in.  Production arms always set g_console_sess (unoauto_gate_arm refuses
     * without a valid one), so this is not a hole in the real path. */
    if (g_lockout) { g_lockout = 0; unoauto_gate_disarm("failed authentication"); return; }
    if (g_armed && g_console_sess && !unosec_session_valid(g_console_sess))
        unoauto_gate_disarm("owner session ended");
}

/* ===========================================================================
 * The link side.
 * ======================================================================== */

/* Production semantics in a debug build, for the regression gate.  Read once:
 * DEBUG.CFG cannot change under us mid-boot, and this is called per verb. */
static int auth_forced(void)
{
#ifdef UNO_DEBUG
    static int cached = -1;
    if (cached < 0) cached = pc64_stress_cfg_flag("urc-auth") > 0;
    return cached;
#else
    return 1;
#endif
}

void unoauto_gate_boot(void)
{
#ifdef UNO_DEBUG
    /* `urc-auth=<token>` in DEBUG.CFG: run the PRODUCTION auth + per-verb rules
     * in a debug build, with the token supplied by the config instead of minted
     * and shown on a screen.  That is the whole reason this hook exists - a
     * headless QEMU gate has no console user to arm the channel and no way to
     * read a random token, so without it the entire security path would ship
     * untested and only ever run on a machine with a human in front of it.
     *
     * It arms OBSERVE|DRIVE and deliberately NOT SYSTEM, so the gate can prove
     * all three outcomes in one boot: allowed, refused-for-lack-of-power, and
     * refused-for-lack-of-auth.  No unosecure involvement - there is no session
     * to escalate from - which is also why this is debug-only: the key does
     * nothing in a production image, where auth_forced() ignores DEBUG.CFG.
     */
    char t[UNOAUTO_TOKEN_BUF];
    if (g_armed || !auth_forced()) return;
    if (pc64_stress_cfg_value("urc-auth", t, (int)sizeof t) <= 0) return;
    strcpy_n(g_token, t, (int)sizeof g_token);
    g_powers = UNOAUTO_P_OBSERVE | UNOAUTO_P_DRIVE;
    g_owner  = UNOSEC_UID_SYSTEM;
    strcpy_n(g_owner_name, "debug-cfg", (int)sizeof g_owner_name);
    g_armed  = 1;
    unoauto_log(UA_CH_SCRIPT, "urc: DEBUG.CFG urc-auth - production gate armed "
                              "(observe+drive, token from config)");
#endif
}

int unoauto_gate_open(void)
{
#ifdef UNO_DEBUG
    if (!auth_forced()) return 1;      /* the harness arms itself from DEBUG.CFG */
#endif
    return g_armed;
}

int unoauto_gate_needs_auth(void) { return auth_forced(); }

int unoauto_gate_authed(void)
{
    if (!auth_forced()) return 1;
    return g_authed;
}

void unoauto_gate_link_reset(void)
{
    /* The link dropped.  Deauthenticate, but stay ARMED: in listen mode the
     * operator expects to reconnect with the same token, and re-arming would
     * mean walking back to the machine. */
    g_authed = 0;
}

int unoauto_gate_auth(const char *token)
{
    int i, diff = 0;
    if (!auth_forced()) { g_authed = 1; return 1; }
    if (!g_armed || !token) return 0;

    /* Already at (or past) the strike limit, or a lockout is pending: do NOT
     * compare, and stand the channel down INLINE rather than deferring to the
     * next tick.  The deferral was the hole: drain_rx dispatches every
     * newline-delimited line in one socket read before the tick runs, so an
     * attacker could PIPELINE hundreds of `auth` lines into a single window and
     * a correct guess sitting AFTER the third wrong one still authenticated.
     * Refusing (and disarming) before any comparison closes that window - no
     * further guess in the same read can slip through, and the channel is gone
     * by the time control returns to drain_rx. */
    if (g_lockout || g_badauth >= BADAUTH_MAX) {
        unoauto_gate_disarm("failed authentication");
        return 0;
    }

    /* Constant-time over the fixed token length: compare every byte whatever
     * happens, so timing does not leak the length of a correct prefix. */
    for (i = 0; i < UNOAUTO_TOKEN_CHARS; i++) {
        char t = token[i];
        diff |= (int)((unsigned char)g_token[i] ^ (unsigned char)t);
        if (!t) { diff |= 1; }             /* short token: mismatch, keep going */
    }
    if (token[UNOAUTO_TOKEN_CHARS]) diff |= 1;      /* trailing junk */

    if (diff) {
        g_badauth++;
        unoauto_log(UA_CH_SCRIPT, "urc: auth FAILED (%d/%d)", g_badauth, BADAUTH_MAX);
        unosec_audit(USC_CAP_AUTOMATE_OBSERVE, "urc:auth", 0);
        /* Three strikes stands the channel down entirely.  A plaintext LAN
         * protocol must not be a brute-force oracle: the operator can re-arm at
         * the console, an attacker on the wire cannot.
         *
         * DEFERRED to the next frame, not done here: disarming closes the link,
         * and the caller has not yet queued - let alone flushed - the "auth
         * failed" response.  Tearing the socket down inside the handler makes
         * the third attempt answer with a dropped connection instead of a
         * refusal, which is worse for an operator who simply mistyped and tells
         * an attacker the same thing anyway. */
        if (g_badauth >= BADAUTH_MAX) g_lockout = 1;
        return 0;
    }

    g_authed  = 1;
    g_badauth = 0;
    unoauto_log(UA_CH_SCRIPT, "urc: link authenticated as %s", g_owner_name);
    unosec_audit(power_cap(g_powers), "urc:auth", 1);
    return 1;
}

int unoauto_gate_verb(const char *verb, const char **why)
{
    const GateRow *r;
    if (why) *why = 0;
    r = gate_find(verb);

    /* AUTHENTICATE FIRST (do not leak console state to a stranger).  The
     * input-locked DRIVE refusal below is a per-verb SIGNAL: it fires only while
     * a security dialog is open at the console.  Run before the auth gate, it
     * would hand an UNAUTHENTICATED peer an oracle - send a DRIVE verb and read
     * "refused (dialog open)" vs "auth-required" to learn whether someone is at
     * a security sheet.  So the auth checks come first: a peer that has not
     * authenticated always gets the same generic denial, whatever is on screen.
     * (Only meaningful when auth is forced; a transparent debug build has no
     * pre-auth peer to protect from.) */
    if (auth_forced()) {
        if (!g_armed)  { if (why) *why = "not-armed";     return 0; }
        if (!g_authed) { if (why) *why = "auth-required"; return 0; }
    }

    /* A security dialog is open at the console.  Refuse the DRIVE class rather
     * than let it inject into a sheet that decides identity and authority - the
     * injection is already dropped at the source (uefi_main.c), but answering
     * "ok" to a click that silently went nowhere is worse than saying no.  READ
     * verbs stay allowed on purpose: the operator should still be able to
     * `screen` grab the dialog and see why the box stopped responding, and the
     * SYSTEM class keeps `reboot` available as the escape hatch from a dialog
     * nobody is there to close.
     *
     * Kept ABOVE the debug transparent return so it still applies in a debug
     * build (this is UI safety, not authorization; metal-caught on the ZimaBlade
     * 2026-08-03 - the injection was dropped correctly but `launch` still
     * answered "launched", which is a lie).  It now sits AFTER the auth gate so
     * it can never be a pre-auth oracle (see above). */
    if (r && (r->power & UNOAUTO_P_DRIVE) && uno_pc64_input_locked()) {
        if (why) *why = "refused (a security dialog is open at the console)";
        return 0;
    }

    if (!auth_forced()) return 1;                /* debug harness: transparent */

    if (!r)        { if (why) *why = "unknown-verb";     return 0; }
    if (!r->power) return 1;                     /* ungated handshake verb */

    if ((g_powers & r->power) != r->power) {
        /* Name the missing power, not just "denied": the operator's next move
         * is to walk to the console and grant it, and they need to know which. */
        if (why) *why = (r->power & UNOAUTO_P_SYSTEM) ? "denied (needs automate.system)"
                      : (r->power & UNOAUTO_P_DRIVE)  ? "denied (needs automate.drive)"
                                                      : "denied (needs automate.observe)";
        unosec_audit(power_cap(r->power), verb, 0);
        return 0;
    }
    /* SYSTEM-tier actions are audited individually: the audit chain is the only
     * record of what a remote operator did to this machine. */
    if (r->power & UNOAUTO_P_SYSTEM) unosec_audit(USC_CAP_AUTOMATE_SYSTEM, verb, 1);
    return 1;
}
