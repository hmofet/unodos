/* ===========================================================================
 * UnoDOS/pc64 - unoxfer's automation verb: `xfer`.
 *
 * ONE entry point, unoxfer_cmd(), the same shape as ssh_dbg_cmd /
 * r8169_dbg_cmd / uno_hw_wdt_cmd.  unoautomate lands one weak stub, one
 * four-line dispatch clause and one GATE[] row; after that everything here is
 * invisible to it, because the sub-verb grammar and the output format are
 * ours.  Nothing in this file asks unoautomate for new machinery.
 *
 * WHY THE VERB EXISTS.  `put` caps a single upload at 8 MB - the RAM staging
 * buffer - and every byte crosses the URC link, which is plaintext, LAN-only,
 * and pumped 512 bytes per tick.  That is right for pushing a 1.5 MB
 * BOOTX64.EFI and wrong for a WAD, a video, or a source tree.  So `xfer`
 * carries no payload at all: it tells the box to FETCH the bytes itself, over
 * a real transfer protocol, straight from the machine that has them.  The cap
 * goes away because the buffer does.
 *
 * PULL AND PUSH RETURN IMMEDIATELY, and that is not a convenience.  A URC
 * command that blocked for the length of a multi-gigabyte transfer would park
 * the dispatcher, stop the box answering, miss the guard's deadline, and get
 * the machine hard-reset by its own dead-man's switch.  So a transfer is a
 * JOB with an id, run on the shell tick, and `status` is how you watch it.
 * ======================================================================== */
#include "unoxfer.h"

int   snprintf(char *, unsigned long, const char *, ...);
unsigned long strlen(const char *);
void *memset(void *, int, unsigned long);
int   uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);

/* ---- text helpers -------------------------------------------------------- */
typedef struct { char *p; int cap, len; } ob;

static void o_s(ob *b, const char *s)
{ while (s && *s && b->len < b->cap - 1) b->p[b->len++] = *s++; b->p[b->len] = 0; }

static void o_n(ob *b, long long v)
{
    char t[24];
    int i = 0, j;
    if (v < 0) { o_s(b, "-"); v = -v; }
    if (!v) { o_s(b, "0"); return; }
    while (v && i < 23) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (j = i - 1; j >= 0; j--) if (b->len < b->cap - 1) b->p[b->len++] = t[j];
    b->p[b->len] = 0;
}

static int word(const char **s, char *out, int cap)
{
    int n = 0;
    while (**s == ' ' || **s == '\t') (*s)++;
    while (**s && **s != ' ' && **s != '\t' && n < cap - 1) out[n++] = *(*s)++;
    out[n] = 0;
    return n;
}

static int eq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int num(const char *s)
{ int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }

/* ---- resolve a target: a saved site name, or a URL ------------------------ */
static int resolve(const char *target, unoxfer_site *s, ob *b)
{
    if (unoxfer_site_get(target, s) == UNOXFER_OK) return 0;
    if (unoxfer_parse_url(target, s) == UNOXFER_OK) return 0;
    o_s(b, "no such site and not a URL: ");
    o_s(b, target);
    o_s(b, "\n(try `xfer sites`, or scp://user@host/path)");
    return -1;
}

/* ===========================================================================
 * The report formats.
 * ======================================================================== */
static void state_word(ob *b, unoxfer_job_state st)
{
    switch (st) {
    case UNOXFER_JOB_PLANNING:  o_s(b, "planning");  break;
    case UNOXFER_JOB_RUNNING:   o_s(b, "running");   break;
    case UNOXFER_JOB_DONE:      o_s(b, "done");      break;
    case UNOXFER_JOB_FAILED:    o_s(b, "failed");    break;
    case UNOXFER_JOB_CANCELLED: o_s(b, "cancelled"); break;
    default:                    o_s(b, "idle");      break;
    }
}

static void status_line(ob *b, int id, const unoxfer_job_stat *st)
{
    o_s(b, "id="); o_n(b, id);
    o_s(b, " state="); state_word(b, st->state);
    o_s(b, " files="); o_n(b, st->files_done);
    o_s(b, "/");       o_n(b, st->files_total);
    o_s(b, " bytes="); o_n(b, (long long)st->bytes_done);
    o_s(b, "/");       o_n(b, (long long)st->bytes_total);
    if (st->bytes_total) {
        o_s(b, " ");
        o_n(b, (long long)((st->bytes_done * 100ull) / st->bytes_total));
        o_s(b, "%");
    }
    if (st->cur[0] && st->state == UNOXFER_JOB_RUNNING) { o_s(b, " cur="); o_s(b, st->cur); }
    if (st->errors) { o_s(b, " errors="); o_n(b, st->errors); }
    if (st->msg[0] && st->state != UNOXFER_JOB_RUNNING) { o_s(b, " - "); o_s(b, st->msg); }
}

static void help(ob *b)
{
    o_s(b,
"xfer - UnoTransfer, the file-transfer subsystem (pc64/UNOXFER.md)\n"
"\n"
"  sites                                   the saved sites\n"
"  site <name> <proto> <host> [port] [user] [key] [root]\n"
"                                          save one (no secret: `key` names an\n"
"                                          unossh key, `ssh keys` lists them)\n"
"  siterm <name>                           forget one\n"
"  caps                                    which protocols this build can use\n"
"  vols                                    the volumes a pull can land on\n"
"  ls <site|url> [path]                    one directory listing\n"
"  pull <site|url> <rpath> <vol> <lpath> [-r]\n"
"  push <site|url> <vol> <lpath> <rpath> [-r]\n"
"                                          START a job; returns an id AT ONCE\n"
"  jobs                                    every job this boot\n"
"  status [id]                             progress\n"
"  cancel <id> | reap <id>                 stop one / release its slot\n"
"  log <id> [from]                         the per-file result list\n"
"  stage [bytes]                           the per-file staging cap\n"
"\n"
"proto: scp sftp http https webdav webdavs tftp local\n"
"A URL may NOT carry a password; name an unossh key instead.");
}

/* ===========================================================================
 * unoxfer_cmd - the whole grammar.
 * ======================================================================== */
int unoxfer_cmd(const char *line, char *out, int cap)
{
    char sub[24], a1[UNOXFER_PATHLEN], a2[UNOXFER_PATHLEN], a3[64], a4[UNOXFER_PATHLEN];
    unoxfer_site site;
    ob b;
    const char *s = line ? line : "";

    b.p = out; b.cap = cap; b.len = 0;
    if (out && cap > 0) out[0] = 0;
    word(&s, sub, (int)sizeof sub);

    if (!sub[0] || eq(sub, "help")) { help(&b); return b.len; }

    /* ---- caps: what this build can actually do ---------------------------
     * Asked BEFORE anything is attempted, so a client greys out what it cannot
     * use instead of discovering the boundary by tripping it.  `caps` on the
     * URC channel itself makes exactly this argument. */
    if (eq(sub, "caps")) {
        int p;
        for (p = 1; p < UNOXFER_PROTO_COUNT; p++) {
            o_s(&b, unoxfer_proto_name((unoxfer_proto)p));
            o_s(&b, unoxfer_proto_ready((unoxfer_proto)p) ? " ready" : " unavailable");
            o_s(&b, "\n");
        }
        o_s(&b, "staging-cap "); o_n(&b, unoxfer_stage_cap());
        o_s(&b, unoxfer_streaming() ? " (streaming: no per-file limit)"
                                    : " (per-file limit; unofs append not linked)");
        o_s(&b, "\nsite-store ");
        o_s(&b, unoxfer_store_persistent() ? "persistent" : "RAM DISK (lost at power-off)");
        return b.len;
    }

    if (eq(sub, "vols")) {
        int n = uno_fs_volumes(), i;
        for (i = 0; i < n; i++) {
            o_n(&b, i); o_s(&b, " "); o_s(&b, uno_fs_volume_name(i));
            o_s(&b, "\n");
        }
        if (b.len) b.p[--b.len] = 0;
        return b.len;
    }

    /* ---- sites ----------------------------------------------------------- */
    if (eq(sub, "sites")) {
        char name[UNOXFER_NAMELEN];
        int i = 0, n;
        for (;;) {
            n = unoxfer_site_list(i, name, (int)sizeof name);
            if (n < 0) break;
            if (unoxfer_site_get(name, &site) == UNOXFER_OK) {
                o_s(&b, name);
                o_s(&b, " "); o_s(&b, unoxfer_proto_name(site.proto));
                o_s(&b, " "); o_s(&b, site.user[0] ? site.user : "-");
                o_s(&b, "@");  o_s(&b, site.host);
                o_s(&b, ":");  o_n(&b, site.port ? site.port
                                                 : unoxfer_proto_port(site.proto));
                if (site.key[0]) { o_s(&b, " key="); o_s(&b, site.key); }
                o_s(&b, "\n");
            }
            i++;
        }
        if (!b.len) o_s(&b, "(no sites saved)");
        else b.p[--b.len] = 0;
        return b.len;
    }

    if (eq(sub, "site")) {
        char proto[16];
        memset(&site, 0, sizeof site);
        if (!word(&s, a1, (int)sizeof a1) || !word(&s, proto, (int)sizeof proto) ||
            !word(&s, a2, (int)sizeof a2)) {
            o_s(&b, "usage: xfer site <name> <proto> <host> [port] [user] [key] [root]");
            return -1;
        }
        site.proto = unoxfer_proto_parse(proto);
        if (site.proto == UNOXFER_NONE) {
            o_s(&b, "unknown protocol: "); o_s(&b, proto);
            o_s(&b, " (see `xfer caps`)");
            return -1;
        }
        {
            int i;
            for (i = 0; a1[i] && i < UNOXFER_NAMELEN - 1; i++) site.name[i] = a1[i];
            site.name[i] = 0;
            for (i = 0; a2[i] && i < UNOXFER_HOSTLEN - 1; i++) site.host[i] = a2[i];
            site.host[i] = 0;
        }
        if (word(&s, a3, (int)sizeof a3)) site.port = num(a3);
        if (word(&s, a3, (int)sizeof a3)) {
            int i;
            for (i = 0; a3[i] && i < UNOXFER_NAMELEN - 1; i++) site.user[i] = a3[i];
            site.user[i] = 0;
        }
        if (word(&s, a3, (int)sizeof a3)) {
            int i;
            for (i = 0; a3[i] && i < UNOXFER_NAMELEN - 1; i++) site.key[i] = a3[i];
            site.key[i] = 0;
        }
        if (!word(&s, a4, (int)sizeof a4)) { a4[0] = '/'; a4[1] = 0; }
        {
            int i;
            for (i = 0; a4[i] && i < UNOXFER_PATHLEN - 1; i++) site.root[i] = a4[i];
            site.root[i] = 0;
        }
        if (unoxfer_site_set(&site) != UNOXFER_OK) {
            o_s(&b, "could not save the site (store full, or read-only volume)");
            return -1;
        }
        o_s(&b, "saved ");
        o_s(&b, site.name);
        if (!unoxfer_store_persistent())
            o_s(&b, " - WARNING: the store is on the RAM disk and dies at power-off");
        return b.len;
    }

    if (eq(sub, "siterm")) {
        if (!word(&s, a1, (int)sizeof a1)) { o_s(&b, "usage: xfer siterm <name>"); return -1; }
        if (unoxfer_site_delete(a1) != UNOXFER_OK) { o_s(&b, "no such site"); return -1; }
        o_s(&b, "forgotten");
        return b.len;
    }

    /* ---- one listing ------------------------------------------------------
     * The one BLOCKING operation here, and deliberately: a directory listing
     * is a bounded round trip whose answer is the reason the command was sent.
     * Making it a job would mean two commands to see a directory. */
    if (eq(sub, "ls")) {
        static unoxfer_ent ents[64];
        unoxfer_client *c;
        char err[160];
        int n, total = 0, i;
        if (!word(&s, a1, (int)sizeof a1)) { o_s(&b, "usage: xfer ls <site|url> [path]"); return -1; }
        if (resolve(a1, &site, &b) < 0) return -1;
        if (!word(&s, a2, (int)sizeof a2)) {
            int k;
            for (k = 0; site.root[k] && k < UNOXFER_PATHLEN - 1; k++) a2[k] = site.root[k];
            a2[k] = 0;
            if (!a2[0]) { a2[0] = '/'; a2[1] = 0; }
        }
        c = unoxfer_open(&site, err, (int)sizeof err);
        if (!c) { o_s(&b, err); return -1; }
        n = unoxfer_list(c, a2, ents, 64, &total);
        if (n < 0) { o_s(&b, unoxfer_error(c)); unoxfer_close(c); return -1; }
        for (i = 0; i < n; i++) {
            o_s(&b, ents[i].is_dir ? "d " : "- ");
            o_n(&b, (long long)ents[i].size);
            o_s(&b, " ");
            o_s(&b, ents[i].name);
            o_s(&b, "\n");
        }
        /* Truncation is REPORTED, never silent: a listing that is missing rows
         * and does not say so is how a caller decides a directory is smaller
         * than it is. */
        if (total > n) { o_s(&b, "... "); o_n(&b, total - n); o_s(&b, " more not shown\n"); }
        if (b.len) b.p[--b.len] = 0;
        else o_s(&b, "(empty)");
        unoxfer_close(c);
        return b.len;
    }

    /* ---- the transfers ---------------------------------------------------- */
    if (eq(sub, "pull") || eq(sub, "push")) {
        /* NOT sub[1]: "pull" and "push" both carry 'u' there, so the obvious
         * one-character test makes every pull a push - which then fails as
         * "this protocol cannot upload" and points at the backend.  Compare
         * the whole word. */
        int push = eq(sub, "push");
        int vol, recurse = 0, id;
        char flag[8];
        if (!word(&s, a1, (int)sizeof a1)) { o_s(&b, "usage: xfer pull <site|url> <rpath> <vol> <lpath> [-r]"); return -1; }
        if (resolve(a1, &site, &b) < 0) return -1;

        if (push) {
            if (!word(&s, a3, (int)sizeof a3) || !word(&s, a2, (int)sizeof a2) ||
                !word(&s, a4, (int)sizeof a4)) {
                o_s(&b, "usage: xfer push <site|url> <vol> <lpath> <rpath> [-r]");
                return -1;
            }
            vol = num(a3);
        } else {
            if (!word(&s, a4, (int)sizeof a4) || !word(&s, a3, (int)sizeof a3) ||
                !word(&s, a2, (int)sizeof a2)) {
                o_s(&b, "usage: xfer pull <site|url> <rpath> <vol> <lpath> [-r]");
                return -1;
            }
            vol = num(a3);
        }
        if (word(&s, flag, (int)sizeof flag))
            recurse = eq(flag, "-r") || eq(flag, "-R") || eq(flag, "--recursive");
        if (vol < 0 || vol >= uno_fs_volumes()) {
            o_s(&b, "no such volume (see `xfer vols`)");
            return -1;
        }

        id = push ? unoxfer_job_push(&site, vol, a2, a4, recurse)
                  : unoxfer_job_pull(&site, a4, vol, a2, recurse);
        if (id < 0) {
            /* A job that failed at START never got an id, so its reason has to
             * come back here rather than through `status`. */
            o_s(&b, "could not start: ");
            o_s(&b, unoxfer_strerror(id));
            return -1;
        }
        o_s(&b, "id="); o_n(&b, id);
        o_s(&b, recurse ? " recursive" : " single-file");
        o_s(&b, " - planning; poll `xfer status "); o_n(&b, id); o_s(&b, "`");
        return b.len;
    }

    if (eq(sub, "jobs")) {
        unoxfer_job_stat st;
        int i, any = 0;
        for (i = 0; i < UNOXFER_MAXJOB; i++)
            if (unoxfer_job_status(i, &st) == UNOXFER_OK) {
                status_line(&b, i, &st);
                o_s(&b, "\n");
                any = 1;
            }
        if (!any) o_s(&b, "(no jobs)");
        else b.p[--b.len] = 0;
        return b.len;
    }

    if (eq(sub, "status")) {
        unoxfer_job_stat st;
        if (!word(&s, a3, (int)sizeof a3)) {
            int i, any = 0;
            for (i = 0; i < UNOXFER_MAXJOB; i++)
                if (unoxfer_job_status(i, &st) == UNOXFER_OK) {
                    status_line(&b, i, &st); o_s(&b, "\n"); any = 1;
                }
            if (!any) o_s(&b, "(no jobs)"); else b.p[--b.len] = 0;
            return b.len;
        }
        if (unoxfer_job_status(num(a3), &st) != UNOXFER_OK) { o_s(&b, "no such job"); return -1; }
        status_line(&b, num(a3), &st);
        return b.len;
    }

    if (eq(sub, "cancel")) {
        if (!word(&s, a3, (int)sizeof a3)) { o_s(&b, "usage: xfer cancel <id>"); return -1; }
        if (unoxfer_job_cancel(num(a3)) != UNOXFER_OK) { o_s(&b, "no such job"); return -1; }
        o_s(&b, "cancelled");
        return b.len;
    }

    if (eq(sub, "reap")) {
        if (!word(&s, a3, (int)sizeof a3)) { o_s(&b, "usage: xfer reap <id>"); return -1; }
        unoxfer_job_reap(num(a3));
        o_s(&b, "reaped");
        return b.len;
    }

    /* ---- the per-file log, in bounded slices ------------------------------
     * unoautomate's report buffer is 4 KB and its TX buffer 8 KB, and a job's
     * log is unbounded.  So `log` takes a starting line and stops when the
     * buffer is nearly full, telling the caller where to resume - the same
     * idiom `readsec` and `screen read` use, and no new streaming machinery
     * asked of unoautomate. */
    if (eq(sub, "log")) {
        char lbuf[288];
        int id, from = 0, n, printed = 0;
        if (!word(&s, a3, (int)sizeof a3)) { o_s(&b, "usage: xfer log <id> [from]"); return -1; }
        id = num(a3);
        if (word(&s, a3, (int)sizeof a3)) from = num(a3);
        for (n = from; ; n++) {
            int len = unoxfer_job_log(id, n, lbuf, (int)sizeof lbuf);
            if (len < 0) break;
            if (b.len + len + 40 > b.cap) {
                o_s(&b, "... more from "); o_n(&b, n);
                o_s(&b, " (xfer log "); o_n(&b, id); o_s(&b, " "); o_n(&b, n); o_s(&b, ")");
                return b.len;
            }
            o_s(&b, lbuf);
            o_s(&b, "\n");
            printed++;
        }
        if (!printed) { o_s(&b, from ? "(no more)" : "no such job, or nothing planned yet");
                        return from ? b.len : -1; }
        b.p[--b.len] = 0;
        return b.len;
    }

    if (eq(sub, "stage")) {
        if (word(&s, a3, (int)sizeof a3)) unoxfer_stage_set_cap((long long)num(a3));
        o_s(&b, "staging-cap "); o_n(&b, unoxfer_stage_cap());
        o_s(&b, unoxfer_streaming() ? " (streaming)" : " (per-file limit)");
        return b.len;
    }

    o_s(&b, "unknown sub-verb: "); o_s(&b, sub);
    o_s(&b, " (try `xfer help`)");
    return -1;
}
