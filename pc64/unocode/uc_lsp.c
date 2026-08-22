/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_lsp.c - the Language Server Protocol client (UCD-22).
 *
 * See uc_lsp.h for what this is for.  This file is the machinery: framing,
 * dispatch, document sync and the restart policy.  Five things in here are less
 * obvious than they look and are worth reading before changing anything.
 *
 * THE OUTGOING QUEUE IS NOT OPTIONAL.  stdin is non-blocking (see uc_proc.h),
 * so a write can be accepted in part.  Everything sent goes on a per-server
 * byte queue and is drained from the front each tick; nothing is ever composed
 * straight into the pipe.  Dropping the remainder of a half-written header
 * corrupts the stream permanently, and in a way that reads as "the server has
 * gone quiet" rather than as an error.
 *
 * A SERVER ASKS THE CLIENT QUESTIONS AND WAITS FOR THE ANSWERS.  pyright will
 * not finish starting until `workspace/configuration` is answered, and the
 * answer has to be an ARRAY with one entry per item asked about - a bare null
 * is a protocol error and it stops there.  clangd wants a progress token
 * created before it reports progress.  So every inbound message carrying both
 * an `id` and a `method` gets a reply, and the default reply is a null result
 * rather than MethodNotFound: an unknown request refused politely is
 * survivable, and one that is never answered is not.
 *
 * A PARSE TREE DIES WHEN ITS MESSAGE DOES.  uc_json's arena is freed whole, so
 * a reply handler that wants to keep something has to keep the ROOT.  The
 * initialize handler is the one that does - the capabilities object has to
 * outlive the message that carried it - so a reply callback may call
 * reply_retain() to take ownership of the tree instead of letting dispatch free
 * it.  Copying a subtree is not an option: an arena has no way to re-home a
 * node into another arena.
 *
 * DOCUMENT VERSIONS COME FROM UcDoc.rev, WHICH IS NOT THE VERSION THE SERVER
 * SEES.  rev counts text changes since the document was created; LSP wants a
 * number that rises once per didChange.  They move at different rates - rev on
 * every keystroke, a didChange after the typing stops - so each synced document
 * carries both: `rev` to notice a change, `version` to number it.
 *
 * THE CHANGE DEBOUNCE IS WHAT MAKES FULL SYNC AFFORDABLE.  A didChange carries
 * the whole buffer; one per keystroke would push a megabyte a second at a
 * compiler that reparsed on each.  Changes wait for UC_LSP_QUIET_MS of silence.
 * Saving flushes immediately, because a save is a deliberate "I am done" and
 * waiting after one looks broken.
 * ======================================================================== */
#include "unocode.h"
#include "uc_lsp.h"
#include "uc_proc.h"

#define UC_LSP_MAX       4        /* distinct servers at once                */
#define UC_LSP_PEND      48       /* requests outstanding per server         */
#define UC_LSP_SYNC      UC_DOC_MAX
#define UC_LSP_LISTEN    4
#define UC_LSP_QUIET_MS  300      /* typing silence before a didChange       */
#define UC_LSP_INIT_MS   20000    /* a server that has not initialized by now */
#define UC_LSP_STABLE_MS 60000    /* READY this long and the backoff resets  */
#define UC_LSP_RXMAX     (8 * 1024 * 1024)

typedef struct {
    int  id;
    UcLspReplyFn cb;
    void *user;
} UcLspPend;

typedef struct {
    UcDoc   *doc;             /* NULL = free slot                            */
    unsigned rev;             /* d->rev when the text was last sent          */
    unsigned pending_rev;     /* d->rev seen but not yet sent                */
    unsigned long quiet_at;   /* when the debounce expires (0 = nothing due) */
    int      version;         /* the LSP document version                    */
    int      opened;          /* didOpen has been sent on THIS server         */
    char     uri[UC_FULL_MAX + 16];
    /* The document's identity, COPIED rather than read back through `doc`.
     * A record has to be able to clean up after the document it describes has
     * already been closed - and by then the pointer is a slot that means some
     * other file (see sync_reresolve).  Copying three fields is cheaper than
     * the class of bug that reading them late invites. */
    int      vol;
    char     dir[UC_PATH_MAX];
    char     name[UC_NAME_MAX];
} UcLspSync;

struct UcLsp {
    char      cmd[256];       /* the command line, and the identity          */
    char      root[UC_FULL_MAX];
    char      root_uri[UC_FULL_MAX + 16];
    uc_proc  *proc;
    int       state;
    int       next_id;
    UcLspPend pend[UC_LSP_PEND];
    int       npend;
    char     *rx; int rxlen, rxcap;        /* framing buffer                 */
    char     *tx; int txlen, txcap, txat;  /* outgoing queue                 */
    UcJson   *caps_root;                   /* the retained initialize reply  */
    UcJson   *caps;                        /* a node inside caps_root        */
    UcLspSync sync[UC_LSP_SYNC];
    int       restarts, fails;
    unsigned long started_at, retry_at, ready_at;
    int       shutting_down;
};

static UcLsp g_srv[UC_LSP_MAX];
static int   g_nsrv;
static int   g_ch = -1;                    /* the Output channel             */
static int   g_inited;
static struct { UcLspNotifyFn fn; void *user; } g_listen[UC_LSP_LISTEN];
static int   g_nlisten;

/* the tree the reply callback currently running was given, and whether it
 * asked to keep it (see the header comment) */
static UcJson *g_reply_root;
static int     g_reply_kept;
static void reply_retain(void) { g_reply_kept = 1; }

/* defined with the diagnostics, below the sync table it has to read */
static void diag_publish(UcLsp *s, UcJson *params);
static void server_source(UcLsp *s, char *out, int cap);

/* ---- logging --------------------------------------------------------------
 * One channel, "Language Server", and a trace setting that decides whether the
 * traffic goes in it.  The traffic is the only way to diagnose a server that
 * starts and then does nothing, which is the failure this protocol produces
 * most often. */
static void lsp_log(const char *a, const char *b)
{
    /* Composed into ONE buffer rather than written in two parts: the channel
     * ends a line at every write that does not end in a newline, so a prefix
     * and its message written separately land on separate lines. */
    char line[240];
    if (g_ch < 0) g_ch = uc_output_channel("Language Server");
    if (g_ch < 0) return;
    uc_scpy(line, a ? a : "", (int)sizeof line);
    if (b) uc_scat(line, b, (int)sizeof line);
    uc_scat(line, "\n", (int)sizeof line);
    uc_output_write(g_ch, line);
}

/* Tracing is a SETTING, plus an override the headless driver can set without
 * touching the user's settings.json.  A gate that had to write a real config
 * file to test would be a gate that damaged the machine it ran on. */
static int g_force_trace;
void uc_lsp_trace_on(void) { g_force_trace = 1; }
static int lsp_tracing(void) { return g_force_trace || uc_cfg_bool("lsp.trace"); }

static void lsp_trace(const char *dir, const char *s, int n)
{
    char head[400];
    int i = 0;
    if (!lsp_tracing()) return;
    while (i < n && i < (int)sizeof head - 1) { head[i] = s[i]; i++; }
    head[i] = 0;
    lsp_log(dir, head);
}

/* ---- growable byte buffers ------------------------------------------------
 * The core's idiom: realloc into a temp, keep the old buffer on failure. */
static int buf_add(char **buf, int *len, int *cap, const char *s, int n)
{
    if (n < 0) n = 0;
    if (*len + n + 1 > *cap) {
        int want = (*cap ? *cap : 1024);
        char *p;
        while (want < *len + n + 1) want *= 2;
        p = (char *)realloc(*buf, (unsigned long)want);
        if (!p) return 0;
        *buf = p;
        *cap = want;
    }
    if (n) memcpy(*buf + *len, s, (unsigned long)n);
    *len += n;
    (*buf)[*len] = 0;
    return 1;
}

/* append a NUL-terminated literal - the shape nine callers out of ten want */
static int buf_str(char **b, int *len, int *cap, const char *s)
{
    return buf_add(b, len, cap, s, s ? (int)strlen(s) : 0);
}

static int buf_int(char **b, int *len, int *cap, long v)
{
    char num[24];
    uc_itoa(num, v);
    return buf_str(b, len, cap, num);
}

int uc_lsp_esc(char *out, int cap, const char *s)
{
    return uc_json_esc(out, cap, s ? s : "");
}

/* The one string compare this file needs and the core does not have.  Header
 * field names are case-insensitive and clangd spells it "Content-Length" while
 * a proxy may not. */
static int hdr_eq(const char *s, const char *lower, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return 0;
    }
    return 1;
}

/* ---- paths and URIs -------------------------------------------------------
 * The editor addresses files by (volume, directory, name); a server wants a
 * URI.  uc_proc_workdir() is the only bridge across that, and it lives in the
 * process seam because the answer only exists on a platform with processes. */
static int path_of(UcDoc *d, char *out, int cap)
{
    char dir[UC_FULL_MAX];
    if (cap > 0) out[0] = 0;
    if (!d || d->vol < 0 || !d->name[0]) return 0;
    if (!uc_proc_workdir(d->vol, d->dir, dir, (int)sizeof dir)) return 0;
    uc_scpy(out, dir, cap);
    uc_scat(out, "/", cap);
    uc_scat(out, d->name, cap);
    return out[0] ? 1 : 0;
}

/* file:// with the path percent-encoded.  '/' and ':' are left alone: a drive
 * letter that came back as file:///C%3A/... is not a path any server accepts,
 * and encoding the separators would turn the whole path into one segment. */
static int uri_of_path(const char *path, char *out, int cap)
{
    static const char hex[] = "0123456789ABCDEF";
    int n, i;
    if (cap > 0) out[0] = 0;
    if (!path || !path[0] || cap < 16) return 0;
    uc_scpy(out, "file://", cap);
    n = (int)strlen(out);
    /* an absolute Windows path starts at a drive letter, and a URI needs the
     * root slash in front of it that the path itself does not have */
    if (path[0] != '/' && n < cap - 1) out[n++] = '/';
    for (i = 0; path[i] && n < cap - 4; i++) {
        unsigned char c = (unsigned char)path[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '/' || c == ':' || c == '-' || c == '.' || c == '_' ||
            c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[(c >> 4) & 15];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
    return 1;
}

int uc_lsp_doc_uri(UcDoc *d, char *out, int cap)
{
    char path[UC_FULL_MAX];
    if (!path_of(d, path, (int)sizeof path)) { if (cap > 0) out[0] = 0; return 0; }
    return uri_of_path(path, out, cap);
}

/* ---- which server serves which language -----------------------------------
 * Built-in defaults, overridable from settings.json:
 *
 *     "lsp.servers": { "python": "pyright-langserver --stdio", "rust": "" }
 *
 * An empty string turns one language off, which is the only way to say "not
 * this one" without turning the whole feature off. */
static const struct { const char *lang, *cmd; } kServers[] = {
    { "c",          "clangd --log=error --background-index" },
    { "cpp",        "clangd --log=error --background-index" },
    { "python",     "pyright-langserver --stdio" },
    { "typescript", "typescript-language-server --stdio" },
    { "javascript", "typescript-language-server --stdio" },
    { "rust",       "rust-analyzer" },
    { "go",         "gopls" },
    { 0, 0 }
};

static const char *server_cmd_for(const char *lang_id)
{
    UcJson *o, *m;
    int i;
    if (!lang_id || !lang_id[0]) return 0;
    o = uc_cfg_raw("lsp.servers");
    if (o && o->type == UJ_OBJ) {
        m = uc_json_member(o, lang_id);
        if (m && m->type == UJ_STR) return (m->str && m->str[0]) ? m->str : 0;
    }
    for (i = 0; kServers[i].lang; i++)
        if (!strcmp(kServers[i].lang, lang_id)) return kServers[i].cmd;
    return 0;
}

static const char *lang_id_of(UcDoc *d)
{
    UcLang *l;
    if (!d) return 0;
    l = uc_lang_at(d->lang);
    return l ? l->id : 0;
}

/* ---- sending --------------------------------------------------------------
 * Everything goes through here: header, body, queue.  Never straight into the
 * pipe, so a short write is a retry rather than a corrupted stream. */
static void send_raw(UcLsp *s, const char *body, int n)
{
    char hdr[64];
    if (!s || !s->proc || n <= 0) return;
    snprintf(hdr, sizeof hdr, "Content-Length: %d\r\n\r\n", n);
    if (!buf_add(&s->tx, &s->txlen, &s->txcap, hdr, (int)strlen(hdr))) return;
    if (!buf_add(&s->tx, &s->txlen, &s->txcap, body, n)) return;
    lsp_trace("-> ", body, n);
}

void uc_lsp_notify(UcLsp *s, const char *method, const char *params)
{
    char *b = 0; int len = 0, cap = 0;
    if (!s || !s->proc || !method) return;
    buf_str(&b, &len, &cap, "{\"jsonrpc\":\"2.0\",\"method\":\"");
    buf_str(&b, &len, &cap, method);
    buf_str(&b, &len, &cap, "\",\"params\":");
    buf_str(&b, &len, &cap, (params && params[0]) ? params : "null");
    buf_str(&b, &len, &cap, "}");
    if (b) { send_raw(s, b, len); free(b); }
}

int uc_lsp_request(UcLsp *s, const char *method, const char *params,
                   UcLspReplyFn cb, void *user)
{
    char *b = 0; int len = 0, cap = 0, id;
    if (!s || !s->proc || !method) return 0;
    if (s->npend >= UC_LSP_PEND) {
        lsp_log("too many requests outstanding, dropping ", method);
        return 0;
    }
    id = s->next_id++;
    buf_str(&b, &len, &cap, "{\"jsonrpc\":\"2.0\",\"id\":");
    buf_int(&b, &len, &cap, id);
    buf_str(&b, &len, &cap, ",\"method\":\"");
    buf_str(&b, &len, &cap, method);
    buf_str(&b, &len, &cap, "\",\"params\":");
    buf_str(&b, &len, &cap, (params && params[0]) ? params : "null");
    buf_str(&b, &len, &cap, "}");
    if (!b) return 0;
    send_raw(s, b, len);
    free(b);
    s->pend[s->npend].id = id;
    s->pend[s->npend].cb = cb;
    s->pend[s->npend].user = user;
    s->npend++;
    return id;
}

/* A reply to a request the SERVER made.  `id_text` and `result` are JSON. */
static void send_reply(UcLsp *s, const char *id_text, const char *result)
{
    char *b = 0; int len = 0, cap = 0;
    buf_str(&b, &len, &cap, "{\"jsonrpc\":\"2.0\",\"id\":");
    buf_str(&b, &len, &cap, id_text);
    buf_str(&b, &len, &cap, ",\"result\":");
    buf_str(&b, &len, &cap, result);
    buf_str(&b, &len, &cap, "}");
    if (b) { send_raw(s, b, len); free(b); }
}

/* ---- receiving ------------------------------------------------------------ */

static void listeners_fire(UcLsp *s, const char *method, UcJson *params)
{
    int i;
    for (i = 0; i < g_nlisten; i++)
        if (g_listen[i].fn) g_listen[i].fn(s, method, params, g_listen[i].user);
}

static void on_initialized(UcJson *result, UcJson *error, void *user)
{
    UcLsp *s = (UcLsp *)user;
    if (!s) return;
    if (!result || error) {
        lsp_log("initialize failed: ", s->cmd);
        s->state = UC_LSP_DEAD;
        return;
    }
    if (s->caps_root) uc_json_free(s->caps_root);
    s->caps_root = g_reply_root;      /* the whole message, kept for `caps`  */
    s->caps = uc_json_member(result, "capabilities");
    reply_retain();
    s->state = UC_LSP_READY;
    s->ready_at = uno_dbg_uptime_ms();
    uc_lsp_notify(s, "initialized", "{}");
    lsp_log("ready: ", s->cmd);
    uc_repaint();
}

static void dispatch(UcLsp *s, const char *body, int n)
{
    char err[128];
    UcJson *root, *id, *method, *result, *error, *params;
    lsp_trace("<- ", body, n);
    root = uc_json_parse(body, n, err, sizeof err);
    if (!root) { lsp_log("unparseable message: ", err); return; }

    id     = uc_json_member(root, "id");
    method = uc_json_member(root, "method");
    result = uc_json_member(root, "result");
    error  = uc_json_member(root, "error");
    params = uc_json_member(root, "params");

    if (id && method) {
        /* A REQUEST FROM THE SERVER.  It is blocked until this is answered. */
        char idtext[80];
        char *arr = 0; int alen = 0, acap = 0;
        const char *reply = "null";
        const char *m = method->str ? method->str : "";
        if (id->type == UJ_STR) {
            char esc[64];
            uc_lsp_esc(esc, sizeof esc, id->str);
            snprintf(idtext, sizeof idtext, "\"%s\"", esc);
        } else {
            uc_itoa(idtext, (long)id->num);
        }
        if (!strcmp(m, "workspace/configuration")) {
            /* One entry per item, or pyright treats the reply as malformed and
             * never finishes starting.  Empty objects mean "use your own
             * defaults", which is the honest answer until UCD-27 wires the
             * editor's formatting settings through. */
            UcJson *items = params ? uc_json_member(params, "items") : 0;
            int count = (items && items->type == UJ_ARR && items->n > 0)
                        ? items->n : 1, i;
            buf_str(&arr, &alen, &acap, "[");
            for (i = 0; i < count; i++) buf_str(&arr, &alen, &acap, i ? ",{}" : "{}");
            buf_str(&arr, &alen, &acap, "]");
            reply = arr ? arr : "[]";
        }
        send_reply(s, idtext, reply);
        if (arr) free(arr);
        uc_json_free(root);
        return;
    }

    if (id && (result || error)) {
        /* A REPLY.  Find the request, call it back, forget it. */
        int rid = (id->type == UJ_NUM) ? (int)id->num : -1, i;
        for (i = 0; i < s->npend; i++) {
            if (s->pend[i].id != rid) continue;
            if (s->pend[i].cb) {
                UcLspReplyFn cb = s->pend[i].cb;
                void *u = s->pend[i].user;
                s->pend[i] = s->pend[--s->npend];   /* forget it FIRST, so a
                                                     * callback that sends can
                                                     * reuse the slot */
                g_reply_root = root;
                g_reply_kept = 0;
                cb(error ? 0 : result, error, u);
                g_reply_root = 0;
                if (g_reply_kept) { g_reply_kept = 0; return; }
            } else {
                s->pend[i] = s->pend[--s->npend];
            }
            break;
        }
        uc_json_free(root);
        return;
    }

    if (method) {
        const char *m = method->str ? method->str : "";
        if (!strcmp(m, "window/logMessage") || !strcmp(m, "window/showMessage")) {
            const char *text = uc_json_str(params, "message", "");
            if (text && text[0] && lsp_tracing()) lsp_log("server: ", text);
        } else if (!strcmp(m, "textDocument/publishDiagnostics")) {
            diag_publish(s, params);
        }
        listeners_fire(s, m, params);
    }
    uc_json_free(root);
}

/* Pull whole messages out of the framing buffer.  A header can be split across
 * reads and a body across many, so nothing is consumed until all of it is
 * here. */
static void frames_drain(UcLsp *s)
{
    for (;;) {
        int i, hdr_end = -1, clen = -1;
        for (i = 0; i + 3 < s->rxlen; i++) {
            if (s->rx[i] == '\r' && s->rx[i+1] == '\n' &&
                s->rx[i+2] == '\r' && s->rx[i+3] == '\n') { hdr_end = i + 4; break; }
        }
        if (hdr_end < 0) return;                      /* header not complete */
        for (i = 0; i + 15 < hdr_end; i++) {
            if (!hdr_eq(s->rx + i, "content-length:", 15)) continue;
            {
                int j = i + 15, v = 0, any = 0;
                while (j < hdr_end && (s->rx[j] == ' ' || s->rx[j] == '\t')) j++;
                while (j < hdr_end && s->rx[j] >= '0' && s->rx[j] <= '9') {
                    v = v * 10 + (s->rx[j] - '0'); j++; any = 1;
                }
                if (any) clen = v;
            }
            break;
        }
        if (clen < 0 || clen > UC_LSP_RXMAX) {
            /* A header block with no usable length is unrecoverable: there is
             * no way to know where the next one starts.  Drop the connection
             * rather than resynchronise on a guess. */
            lsp_log("no usable Content-Length from ", s->cmd);
            s->rxlen = 0;
            s->state = UC_LSP_DEAD;
            return;
        }
        if (s->rxlen - hdr_end < clen) return;        /* body not complete   */
        dispatch(s, s->rx + hdr_end, clen);
        {
            int used = hdr_end + clen, left = s->rxlen - used;
            if (left > 0) memmove(s->rx, s->rx + used, (unsigned long)left);
            s->rxlen = left;
        }
        if (s->state == UC_LSP_DEAD || !s->proc) return;
    }
}

/* ---- lifecycle ------------------------------------------------------------ */

/* Every restart delay, in one place: 1s, 2s, 4s ... capped at 32s.  A server
 * that cannot start at all must not spin, and one that crashed once must not be
 * punished for a minute. */
/* "has `ms` passed since `since`", written so it cannot answer yes because of a
 * WRAP.  These are unsigned, and a `now` sampled a few instructions before the
 * thing it is timing was started reads as now - since = 0xFFFFFFF..., which is
 * greater than every timeout there is.  That is not a hypothetical: the first
 * attach and the timeout check happen in the same tick, so the very first
 * server ever started was declared unresponsive before it had been given a
 * microsecond, restarted once, and only worked on the second try. */
static int since_ms(unsigned long now, unsigned long then, unsigned long ms)
{
    return now >= then && now - then > ms;
}

static unsigned long backoff_ms(int fails)
{
    int n = fails - 1;
    if (n < 0) n = 0;
    if (n > 5) n = 5;
    return (unsigned long)(1000u << n);
}

static void pending_abandon(UcLsp *s)
{
    int n = s->npend, i;
    s->npend = 0;                       /* before the callbacks, not after   */
    for (i = 0; i < n; i++)
        if (s->pend[i].cb) s->pend[i].cb(0, 0, s->pend[i].user);
}

static void server_stop(UcLsp *s, int polite)
{
    int i;
    if (!s) return;
    if (s->proc && polite) {
        uc_lsp_request(s, "shutdown", "null", 0, 0);
        uc_lsp_notify(s, "exit", "null");
        /* push what we can; the free below is what guarantees it goes away */
        for (i = 0; i < 64 && s->txlen > s->txat; i++) {
            int put = uc_proc_write(s->proc, s->tx + s->txat, s->txlen - s->txat);
            if (put <= 0) break;
            s->txat += put;
        }
    }
    pending_abandon(s);
    /* This server's diagnostics go with it.  A squiggle from a compiler that
     * is no longer running is a claim about a file nobody is still checking,
     * and it stays on screen through every edit that would have fixed it. */
    {   char source[16];
        server_source(s, source, (int)sizeof source);
        uc_problems_clear(source);
    }
    if (s->proc) { uc_proc_free(s->proc); s->proc = 0; }
    if (s->caps_root) { uc_json_free(s->caps_root); s->caps_root = 0; }
    s->caps = 0;
    s->rxlen = 0;
    s->txlen = s->txat = 0;
    s->state = UC_LSP_DEAD;
    /* the documents stay: a restart re-opens them, which is the whole point */
    for (i = 0; i < UC_LSP_SYNC; i++) {
        s->sync[i].opened = 0;
        s->sync[i].version = 0;
        s->sync[i].quiet_at = 0;
    }
}

static void server_died(UcLsp *s, const char *why)
{
    lsp_log(why, s->cmd);
    server_stop(s, 0);
    s->fails++;
    s->restarts++;
    s->retry_at = uno_dbg_uptime_ms() + backoff_ms(s->fails);
}

static const char kCaps[] =
    "{\"general\":{\"positionEncodings\":[\"utf-16\"]},"
    "\"workspace\":{\"workspaceFolders\":true,\"configuration\":true,"
                  "\"applyEdit\":false},"
    "\"textDocument\":{"
      "\"synchronization\":{\"dynamicRegistration\":false,\"didSave\":true,"
                          "\"willSave\":false},"
      "\"publishDiagnostics\":{\"relatedInformation\":false,"
                             "\"versionSupport\":true},"
      "\"completion\":{\"completionItem\":{\"snippetSupport\":false,"
                      "\"documentationFormat\":[\"plaintext\"]}},"
      "\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
      "\"definition\":{},\"references\":{},\"documentSymbol\":{},"
      "\"rename\":{\"prepareSupport\":false},\"formatting\":{}}}";

static void send_initialize(UcLsp *s)
{
    char *b = 0; int len = 0, cap = 0;
    char esc[UC_FULL_MAX * 3];
    uc_lsp_esc(esc, sizeof esc, s->root_uri);
    buf_str(&b, &len, &cap, "{\"processId\":null,\"rootUri\":\"");
    buf_str(&b, &len, &cap, esc);
    buf_str(&b, &len, &cap, "\",\"capabilities\":");
    buf_str(&b, &len, &cap, kCaps);
    buf_str(&b, &len, &cap, ",\"workspaceFolders\":[{\"uri\":\"");
    buf_str(&b, &len, &cap, esc);
    buf_str(&b, &len, &cap, "\",\"name\":\"workspace\"}]}");
    if (b) {
        uc_lsp_request(s, "initialize", b, on_initialized, s);
        free(b);
    }
}

static int server_start(UcLsp *s)
{
    s->proc = uc_proc_spawn_pipes(s->cmd, s->root[0] ? s->root : 0);
    if (!s->proc) {
        lsp_log("could not start: ", uc_proc_error());
        s->state = UC_LSP_DEAD;
        s->fails++;
        s->retry_at = uno_dbg_uptime_ms() + backoff_ms(s->fails);
        return 0;
    }
    s->state = UC_LSP_STARTING;
    s->started_at = uno_dbg_uptime_ms();
    s->next_id = 1;
    s->rxlen = 0;
    s->txlen = s->txat = 0;
    lsp_log("starting: ", s->cmd);
    send_initialize(s);
    return 1;
}

static UcLsp *server_for_cmd(const char *cmd)
{
    UcLsp *s;
    int i;
    for (i = 0; i < g_nsrv; i++)
        if (!strcmp(g_srv[i].cmd, cmd)) return &g_srv[i];
    if (g_nsrv >= UC_LSP_MAX) return 0;
    s = &g_srv[g_nsrv];
    memset(s, 0, sizeof *s);
    uc_scpy(s->cmd, cmd, (int)sizeof s->cmd);
    s->state = UC_LSP_OFF;
    g_nsrv++;
    return s;
}

/* ---- document sync -------------------------------------------------------- */

/* The document is gone - tell the server, take its diagnostics down, and free
 * the slot.  Both callers used to do this inline, and one of them cannot read
 * `k->doc` at all by the time it runs, which is why the record carries the
 * file's identity itself. */
static void sync_retire(UcLsp *s, UcLspSync *k)
{
    char source[16];
    if (k->opened && s->state == UC_LSP_READY) {
        char *b = 0; int len = 0, cap = 0;
        buf_str(&b, &len, &cap, "{\"textDocument\":{\"uri\":\"");
        buf_str(&b, &len, &cap, k->uri);
        buf_str(&b, &len, &cap, "\"}}");
        if (b) { uc_lsp_notify(s, "textDocument/didClose", b); free(b); }
    }
    /* A closed file's squiggles have nowhere left to be drawn, but its rows
     * stay in the Problems panel and its counts stay in the status bar -
     * claims about a file the editor is no longer looking at. */
    server_source(s, source, (int)sizeof source);
    uc_problems_clear_file(k->vol, k->dir, k->name, source);
    memset(k, 0, sizeof *k);
}

static UcLspSync *sync_find(UcLsp *s, UcDoc *d)
{
    int i;
    for (i = 0; i < UC_LSP_SYNC; i++)
        if (s->sync[i].doc == d) return &s->sync[i];
    return 0;
}

static UcLspSync *sync_slot(UcLsp *s, UcDoc *d)
{
    UcLspSync *k = sync_find(s, d);
    int i;
    if (k) return k;
    for (i = 0; i < UC_LSP_SYNC; i++) {
        if (s->sync[i].doc) continue;
        memset(&s->sync[i], 0, sizeof s->sync[i]);
        s->sync[i].doc = d;
        return &s->sync[i];
    }
    return 0;
}

/* The document's text as a JSON string body.  Returns a malloc'd buffer the
 * caller frees, or NULL.  Sized at six bytes per input byte because that is
 * uc_json_esc's worst case (a control character becomes \u00XX). */
static char *text_escaped(UcDoc *d)
{
    int need = (d->len > 0 ? d->len : 0) * 6 + 64;
    char *esc = (char *)malloc((unsigned long)need);
    if (!esc) return 0;
    uc_json_esc(esc, need, d->text ? d->text : "");
    return esc;
}

static void send_did_open(UcLsp *s, UcLspSync *k)
{
    char *b = 0; int len = 0, cap = 0;
    UcDoc *d = k->doc;
    const char *lang = lang_id_of(d);
    char *esc = text_escaped(d);
    if (!esc) return;
    k->version = 1;
    buf_str(&b, &len, &cap, "{\"textDocument\":{\"uri\":\"");
    buf_str(&b, &len, &cap, k->uri);
    buf_str(&b, &len, &cap, "\",\"languageId\":\"");
    buf_str(&b, &len, &cap, (lang && lang[0]) ? lang : "plaintext");
    buf_str(&b, &len, &cap, "\",\"version\":");
    buf_int(&b, &len, &cap, k->version);
    buf_str(&b, &len, &cap, ",\"text\":\"");
    buf_str(&b, &len, &cap, esc);
    buf_str(&b, &len, &cap, "\"}}");
    free(esc);
    if (!b) return;
    uc_lsp_notify(s, "textDocument/didOpen", b);
    free(b);
    k->opened = 1;
    k->rev = k->pending_rev = d->rev;
    k->quiet_at = 0;
}

static void send_did_change(UcLsp *s, UcLspSync *k)
{
    char *b = 0; int len = 0, cap = 0;
    UcDoc *d = k->doc;
    char *esc = text_escaped(d);
    if (!esc) return;
    k->version++;
    buf_str(&b, &len, &cap, "{\"textDocument\":{\"uri\":\"");
    buf_str(&b, &len, &cap, k->uri);
    buf_str(&b, &len, &cap, "\",\"version\":");
    buf_int(&b, &len, &cap, k->version);
    buf_str(&b, &len, &cap, "},\"contentChanges\":[{\"text\":\"");
    buf_str(&b, &len, &cap, esc);
    buf_str(&b, &len, &cap, "\"}]}");
    free(esc);
    if (!b) return;
    uc_lsp_notify(s, "textDocument/didChange", b);
    free(b);
    k->rev = k->pending_rev = d->rev;
    k->quiet_at = 0;
}

UcLsp *uc_lsp_open_doc(UcDoc *d)
{
    const char *cmd;
    UcLsp *s;
    UcLspSync *k;
    char uri[UC_FULL_MAX + 16];
    if (!g_inited || !uc_lsp_available() || !uc_cfg_bool("lsp.enabled")) return 0;
    if (!d || d->vol < 0 || !d->name[0]) return 0;
    cmd = server_cmd_for(lang_id_of(d));
    if (!cmd) return 0;
    if (!uc_lsp_doc_uri(d, uri, (int)sizeof uri)) return 0;
    s = server_for_cmd(cmd);
    if (!s) return 0;
    if (!s->root[0]) {
        uc_proc_workdir(d->vol, "", s->root, (int)sizeof s->root);
        uri_of_path(s->root, s->root_uri, (int)sizeof s->root_uri);
    }
    k = sync_slot(s, d);
    if (!k) return 0;
    uc_scpy(k->uri, uri, (int)sizeof k->uri);
    k->vol = d->vol;
    uc_scpy(k->dir, d->dir, (int)sizeof k->dir);
    uc_scpy(k->name, d->name, (int)sizeof k->name);
    if (s->state == UC_LSP_OFF) server_start(s);
    return s;
}

void uc_lsp_did_save(UcDoc *d)
{
    int i;
    for (i = 0; i < g_nsrv; i++) {
        UcLsp *s = &g_srv[i];
        UcLspSync *k = sync_find(s, d);
        char *b = 0; int len = 0, cap = 0;
        if (!k || !k->opened || s->state != UC_LSP_READY) continue;
        /* flush the debounced text first: a didSave that arrives before the
         * change it saved makes the server diagnose the previous version */
        if (k->rev != d->rev) send_did_change(s, k);
        buf_str(&b, &len, &cap, "{\"textDocument\":{\"uri\":\"");
        buf_str(&b, &len, &cap, k->uri);
        buf_str(&b, &len, &cap, "\"}}");
        if (b) { uc_lsp_notify(s, "textDocument/didSave", b); free(b); }
    }
}

void uc_lsp_close_doc(UcDoc *d)
{
    int i;
    for (i = 0; i < g_nsrv; i++) {
        UcLsp *s = &g_srv[i];
        UcLspSync *k = sync_find(s, d);
        if (!k) continue;
        sync_retire(s, k);
    }
}

/* A UcDoc * IS NOT A STABLE IDENTITY.  Documents live in one array and
 * uc_doc_close() shifts every later one down a slot, so a pointer kept from
 * last frame can come to mean a DIFFERENT file without ever being freed - which
 * would have this client syncing one document's text under another's URI and
 * looking, from the outside, like a server that had gone mad.
 *
 * The URI is the identity that survives.  Each tick every record's pointer is
 * re-derived from it, and a record whose file is no longer open is retired.
 * That also gets save-as right for free: the URI changes, the old record closes
 * itself, and the next attach opens the new one. */
static void sync_reresolve(UcLsp *s)
{
    int i, j, n = uc_doc_count();
    for (i = 0; i < UC_LSP_SYNC; i++) {
        UcLspSync *k = &s->sync[i];
        UcDoc *found = 0;
        if (!k->doc) continue;
        for (j = 0; j < n && !found; j++) {
            char uri[UC_FULL_MAX + 16];
            UcDoc *d = uc_doc_at(j);
            if (!uc_lsp_doc_uri(d, uri, (int)sizeof uri)) continue;
            if (!strcmp(uri, k->uri)) found = d;
        }
        if (found) { k->doc = found; continue; }
        sync_retire(s, k);
    }
}

UcLsp *uc_lsp_for_doc(UcDoc *d)
{
    int i;
    for (i = 0; i < g_nsrv; i++) {
        if (g_srv[i].state != UC_LSP_READY) continue;
        if (sync_find(&g_srv[i], d)) return &g_srv[i];
    }
    return 0;
}

/* ---- positions ------------------------------------------------------------
 * LSP counts a column in UTF-16 CODE UNITS.  UnoCode already counts columns
 * three other ways - bytes, code points (uc_col_of) and visual cells (tabs
 * expanded, wide glyphs two) - and all four agree exactly as long as the text
 * is ASCII, which is how a bug here survives every test anyone writes by hand.
 * They diverge on the first emoji: one code point, TWO UTF-16 units, two cells,
 * four bytes.  A squiggle placed with the wrong unit is not visibly wrong on
 * the line that produced it; it is wrong on every line after the first
 * non-ASCII character, which reads as "the server's ranges are off" rather
 * than as a conversion this side got wrong.
 *
 * utf-16 is what the client asked for in its capabilities, so it is what
 * arrives; if that ever becomes negotiable, this is the only place that knows. */
static int u16_to_byte(const char *line, int len, int u16)
{
    int b = 0, u = 0;
    while (b < len && u < u16) {
        int cp = 0, n = uc_u8_get(line + b, len - b, &cp);
        if (n <= 0) break;
        b += n;
        u += (cp >= 0x10000) ? 2 : 1;   /* astral characters are a surrogate
                                         * PAIR, and cost two units */
    }
    return b;
}

static int byte_to_u16(const char *line, int len, int byte)
{
    int b = 0, u = 0;
    if (byte > len) byte = len;
    while (b < byte) {
        int cp = 0, n = uc_u8_get(line + b, len - b, &cp);
        if (n <= 0) break;
        b += n;
        u += (cp >= 0x10000) ? 2 : 1;
    }
    return u;
}

/* An LSP {line, character} to an offset in `d`.  The line number is 0-based in
 * both, which is the one thing about this that needs no conversion. */
int uc_lsp_pos_to_offset(UcDoc *d, int line, int u16)
{
    int s, e;
    if (!d) return 0;
    if (line < 0) line = 0;
    if (line >= uc_line_count(d)) line = uc_line_count(d) - 1;
    if (line < 0) return 0;
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    return s + u16_to_byte(d->text + s, e - s, u16);
}

int uc_lsp_offset_to_u16(UcDoc *d, int off)
{
    int line, s, e;
    if (!d) return 0;
    line = uc_line_of(d, off);
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    if (off < s) off = s;
    if (off > e) off = e;
    return byte_to_u16(d->text + s, e - s, off - s);
}

/* `{"line":L,"character":C}` for a document offset - every request from UCD-24
 * onwards needs one and none of them should build it by hand. */
int uc_lsp_pos_json(UcDoc *d, int off, char *out, int cap)
{
    char num[24];
    if (!d || cap <= 0) { if (cap > 0) out[0] = 0; return 0; }
    uc_scpy(out, "{\"line\":", cap);
    uc_itoa(num, uc_line_of(d, off));
    uc_scat(out, num, cap);
    uc_scat(out, ",\"character\":", cap);
    uc_itoa(num, uc_lsp_offset_to_u16(d, off));
    uc_scat(out, num, cap);
    uc_scat(out, "}", cap);
    return 1;
}

/* The shape almost every language feature asks in: "this method, about this
 * position, in this document" (UCD-24 onwards).  completion, hover, definition,
 * references, prepareRename and rename all take TextDocumentPositionParams, and
 * a caller that built it itself would be re-deriving the URI and re-doing the
 * UTF-16 conversion at each of six call sites.
 *
 * `extra` is appended inside the object, so it must start with a comma:
 * ",\"context\":{\"triggerKind\":1}".  NULL for the plain form. */
int uc_lsp_request_at(UcDoc *d, const char *method, int off, const char *extra,
                      UcLspReplyFn cb, void *user)
{
    UcLsp *s = uc_lsp_for_doc(d);
    char *b = 0; int len = 0, cap = 0, id;
    char uri[UC_FULL_MAX + 16], pos[64], esc[UC_FULL_MAX * 3];
    if (!s || !method) return 0;
    if (!uc_lsp_doc_uri(d, uri, (int)sizeof uri)) return 0;
    if (!uc_lsp_pos_json(d, off, pos, (int)sizeof pos)) return 0;
    uc_lsp_esc(esc, (int)sizeof esc, uri);
    buf_str(&b, &len, &cap, "{\"textDocument\":{\"uri\":\"");
    buf_str(&b, &len, &cap, esc);
    buf_str(&b, &len, &cap, "\"},\"position\":");
    buf_str(&b, &len, &cap, pos);
    if (extra && extra[0]) buf_str(&b, &len, &cap, extra);
    buf_str(&b, &len, &cap, "}");
    if (!b) return 0;
    id = uc_lsp_request(s, method, b, cb, user);
    free(b);
    return id;
}

/* ---- diagnostics (UCD-23) -------------------------------------------------
 * This lives in the transport's own file rather than beside the Problems model
 * because it is the only code that needs the private sync table: a
 * publishDiagnostics names a URI, and the URI is only a document because this
 * table says which one it is.  Exporting the table to keep files pure would be
 * a worse trade than the one section. */

/* The server's short name, for UcProblem.source - the first word of its
 * command line, so a clear-by-source removes what this server said and not
 * what the build said about the same file. */
static void server_source(UcLsp *s, char *out, int cap)
{
    int i = 0, start = 0, k;
    for (k = 0; s->cmd[k] && s->cmd[k] != ' '; k++)
        if (s->cmd[k] == '/' || s->cmd[k] == '\\') start = k + 1;
    for (i = 0; start + i < k && i < cap - 1; i++) out[i] = s->cmd[start + i];
    out[i] = 0;
}

static UcLspSync *sync_by_uri(UcLsp *s, const char *uri)
{
    int i;
    if (!uri) return 0;
    for (i = 0; i < UC_LSP_SYNC; i++)
        if (s->sync[i].doc && !strcmp(s->sync[i].uri, uri)) return &s->sync[i];
    return 0;
}

static void diag_publish(UcLsp *s, UcJson *params)
{
    const char *uri = uc_json_str(params, "uri", "");
    UcJson *arr = params ? uc_json_member(params, "diagnostics") : 0;
    UcLspSync *k = sync_by_uri(s, uri);
    UcJson *it;
    UcDoc *d;
    char source[16];
    int added = 0;

    /* Diagnostics about a file we never opened are DROPPED, not stored under a
     * guessed path.  clangd publishes for its own `.clangd` config file, and a
     * problem the user cannot click through to is worse than one not shown. */
    if (!k || !arr || arr->type != UJ_ARR) return;
    d = k->doc;
    server_source(s, source, (int)sizeof source);
    uc_problems_clear_file(k->vol, k->dir, k->name, source);

    for (it = arr->child; it; it = it->next) {
        UcProblem p;
        UcJson *range = uc_json_member(it, "range");
        UcJson *a = range ? uc_json_member(range, "start") : 0;
        UcJson *b = range ? uc_json_member(range, "end") : 0;
        int sev, sl, sc, el, ec;
        if (!a) continue;
        /* 64 from one file, so a header that will not parse cannot fill the
         * whole 128-slot store and hide every other file's problems */
        if (added >= 64) break;

        sl = (int)uc_json_num(a, "line", 0);
        sc = (int)uc_json_num(a, "character", 0);
        el = b ? (int)uc_json_num(b, "line", sl) : sl;
        ec = b ? (int)uc_json_num(b, "character", sc) : sc;

        memset(&p, 0, sizeof p);
        sev = (int)uc_json_num(it, "severity", 1);
        p.sev = (sev == 2) ? UC_SEV_WARN :
                (sev >= 3) ? UC_SEV_INFO : UC_SEV_ERROR;
        uc_scpy(p.file, k->name, (int)sizeof p.file);
        uc_scpy(p.dir, k->dir, (int)sizeof p.dir);
        p.vol = k->vol;
        uc_scpy(p.source, source, (int)sizeof p.source);
        uc_scpy(p.msg, uc_json_str(it, "message", ""), (int)sizeof p.msg);

        /* UTF-16 units in, CHARACTER columns out, because that is what
         * uc_offset_of() consumes and what the Problems panel's click already
         * hands it.  Storing UTF-16 here would put the burden on every reader. */
        p.line = sl + 1;
        p.col  = uc_col_of(d, uc_lsp_pos_to_offset(d, sl, sc)) + 1;
        p.end_line = el + 1;
        p.end_col  = uc_col_of(d, uc_lsp_pos_to_offset(d, el, ec)) + 1;
        uc_problems_add(&p);
        added++;
    }
    uc_repaint();
}

/* ---- the tick ------------------------------------------------------------- */

static void tick_io(UcLsp *s)
{
    char buf[8192];
    int n, budget;

    /* Write what the pipe will take.  A short write is normal, not an error:
     * the rest waits here for the next tick. */
    while (s->txlen > s->txat) {
        int put = uc_proc_write(s->proc, s->tx + s->txat, s->txlen - s->txat);
        if (put <= 0) break;
        s->txat += put;
    }
    if (s->txat == s->txlen) { s->txlen = s->txat = 0; }
    else if (s->txat > 65536) {
        memmove(s->tx, s->tx + s->txat, (unsigned long)(s->txlen - s->txat));
        s->txlen -= s->txat;
        s->txat = 0;
    }

    /* stderr, which is where a server that dies before saying anything
     * protocol-shaped explains itself */
    for (budget = 4; budget > 0; budget--) {
        n = uc_proc_read_err(s->proc, buf, (int)sizeof buf - 1);
        if (n <= 0) break;
        buf[n] = 0;
        if (lsp_tracing()) lsp_log("stderr: ", buf);
    }

    /* stdout: the protocol */
    for (budget = 16; budget > 0; budget--) {
        n = uc_proc_read(s->proc, buf, (int)sizeof buf);
        if (n == 0) break;
        if (n < 0) { server_died(s, "server exited: "); return; }
        if (s->rxlen + n > UC_LSP_RXMAX) {
            server_died(s, "dropped an oversized stream from: ");
            return;
        }
        if (!buf_add(&s->rx, &s->rxlen, &s->rxcap, buf, n)) return;
        frames_drain(s);
        if (!s->proc) return;
        if (s->state == UC_LSP_DEAD) { server_died(s, "protocol error from: "); return; }
    }
}

static void tick_sync(UcLsp *s)
{
    unsigned long now = uno_dbg_uptime_ms();
    int i;
    for (i = 0; i < UC_LSP_SYNC; i++) {
        UcLspSync *k = &s->sync[i];
        if (!k->doc) continue;
        if (!k->opened) { send_did_open(s, k); continue; }
        if (k->doc->rev == k->rev) continue;
        /* restart the quiet timer on every change, so it measures silence
         * rather than time since the first keystroke */
        if (k->pending_rev != k->doc->rev) {
            k->pending_rev = k->doc->rev;
            k->quiet_at = now + UC_LSP_QUIET_MS;
        } else if (k->quiet_at && now >= k->quiet_at) {
            send_did_change(s, k);
        }
    }
}

void uc_lsp_tick(void)
{
    unsigned long now;
    int i;
    if (!g_inited || !uc_lsp_available()) return;
    now = uno_dbg_uptime_ms();

    /* Re-derive the document pointers, then attach anything newly open.  This
     * is why there is no uc_lsp_open_doc() call scattered through the open
     * paths: a document reaches the editor by half a dozen routes (the file
     * tree, Quick Open, a drag, a restored session, an extension) and a client
     * that had to be told about each of them would miss one. */
    for (i = 0; i < g_nsrv; i++) sync_reresolve(&g_srv[i]);
    {   int n = uc_doc_count();
        for (i = 0; i < n; i++) uc_lsp_open_doc(uc_doc_at(i));
    }
    now = uno_dbg_uptime_ms();     /* AFTER the attach: it may have started one */

    for (i = 0; i < g_nsrv; i++) {
        UcLsp *s = &g_srv[i];
        if (s->proc) {
            tick_io(s);
            if (!s->proc) continue;
        }
        if (s->state == UC_LSP_STARTING &&
            since_ms(now, s->started_at, UC_LSP_INIT_MS)) {
            server_died(s, "no initialize reply, giving up on: ");
            continue;
        }
        if (s->state == UC_LSP_READY) {
            /* A server that has behaved for a while has earned a clean slate;
             * without this one crash makes every later one wait longer. */
            if (s->fails && since_ms(now, s->ready_at, UC_LSP_STABLE_MS)) s->fails = 0;
            tick_sync(s);
        }
        if (s->state == UC_LSP_DEAD && !s->shutting_down && s->retry_at &&
            now >= s->retry_at) {
            int wanted = 0, j;
            for (j = 0; j < UC_LSP_SYNC; j++) if (s->sync[j].doc) wanted = 1;
            s->retry_at = 0;
            /* nothing open needs it any more - stop restarting a compiler
             * nobody is going to ask a question */
            if (!wanted) { s->state = UC_LSP_OFF; continue; }
            server_start(s);
        }
    }
}

/* ---- setup and teardown --------------------------------------------------- */

int uc_lsp_available(void) { return uc_proc_available(); }

void uc_lsp_init(void)
{
    g_inited = 1;
}

void uc_lsp_shutdown_all(void)
{
    int i;
    for (i = 0; i < g_nsrv; i++) {
        g_srv[i].shutting_down = 1;
        server_stop(&g_srv[i], 1);
        free(g_srv[i].rx);
        free(g_srv[i].tx);
        g_srv[i].rx = g_srv[i].tx = 0;
        g_srv[i].rxcap = g_srv[i].txcap = 0;
    }
    g_nsrv = 0;
    g_inited = 0;
}

void uc_lsp_on_notify(UcLspNotifyFn fn, void *user)
{
    if (g_nlisten >= UC_LSP_LISTEN || !fn) return;
    g_listen[g_nlisten].fn = fn;
    g_listen[g_nlisten].user = user;
    g_nlisten++;
}

UcJson     *uc_lsp_caps(UcLsp *s)     { return s ? s->caps : 0; }
int         uc_lsp_count(void)        { return g_nsrv; }
UcLsp      *uc_lsp_at(int i)          { return (i >= 0 && i < g_nsrv) ? &g_srv[i] : 0; }
int         uc_lsp_state(UcLsp *s)    { return s ? s->state : UC_LSP_OFF; }
const char *uc_lsp_name(UcLsp *s)     { return s ? s->cmd : ""; }
int         uc_lsp_restarts(UcLsp *s) { return s ? s->restarts : 0; }
