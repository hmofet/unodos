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
 * uc_ai.c - the assistant view (UCD-49): a native chat over uc_http.h.
 *
 * A side-bar view, drawn like every other one, holding a conversation with an
 * Anthropic model.  Three design decisions carry the file:
 *
 * THE REQUEST NEVER STOPS THE FRAME.  Everything rides UCD-45..47: the
 * exchange is a uc_http handle pumped from uc_ai_tick(), which uc_frame()
 * calls once per frame, and the SSE handler appends each text delta to the
 * transcript as it arrives.  There is no wait anywhere - which is the entire
 * difference between this and Studio's assistant, whose do_request() freezes
 * the desk until the answer is whole.
 *
 * LOCAL NOTICES ARE NEVER SENT.  Turns are user / assistant / NOTE, and notes
 * (errors, cancellations, hints) exist only on screen.  Dropping them from the
 * request is what leaves two user turns adjacent, which the API rejects - so
 * adjacent same-role turns are merged at build time.  Studio learned this the
 * hard way (UCD-46); the rule is inherited, not rediscovered.
 *
 * AN EDIT IS PROPOSED, NOT PERFORMED.  Clicking Apply on a code block shows
 * what would change - the current selection as the lines leaving, the block as
 * the lines arriving - and only a second, explicit Apply performs it, as ONE
 * undo step.  Reject costs nothing.  A model must not hold the pen.
 *
 * The key comes from uc_secret_get("anthropic.key") - UCD-48's store, set by
 * "AI: Set API Key" - and is fetched at send time, held for the duration of
 * one request build, and not kept anywhere in this file's state.
 * ======================================================================== */
#include "unocode.h"
#include "uc_http.h"
#include "uc_secret.h"

#define AI_TEXT_CAP  (48 * 1024)     /* the whole transcript                 */
#define AI_TURNS     48
#define AI_INPUT_CAP 1024
#define AI_BLOCKS    32              /* code blocks the transcript can hold  */
#define AI_HITS      40              /* clickable regions from the last draw */
#define AI_PROP_CAP  4096            /* the "lines leaving" side of an edit  */
#define AI_CTX_CAP   (24 * 1024)     /* the open file, sent as system context */

enum { AI_USER = 0, AI_ASSIST = 1, AI_NOTE = 2 };

static char g_text[AI_TEXT_CAP];
static int  g_len;
static struct { int start, len; unsigned char role; } g_turn[AI_TURNS];
static int  g_nturn;

static char g_input[AI_INPUT_CAP];
static int  g_inlen;

static uc_http *g_req;               /* the in-flight request, or 0          */
static int  g_scroll;                /* transcript scroll, px                */
static int  g_stick = 1;             /* pinned to the bottom while streaming */
static int  g_content_h;             /* from the last layout walk            */
static int  g_view_h;

/* code blocks, rebuilt by every layout walk */
static struct { int turn, start, len; char lang[14]; } g_blk[AI_BLOCKS];
static int g_nblk;

/* clickable regions, recorded by the last draw (a draw always precedes the
 * click it enables - the frame paints before the mouse can hit it) */
enum { HIT_APPLY = 1, HIT_PROP_APPLY, HIT_PROP_REJECT, HIT_STOP, HIT_NEW };
static struct { UcRect r; int kind, arg; } g_hit[AI_HITS];
static int g_nhit;

/* the pending edit proposal: block index, and the text it would replace */
static int  g_prop = -1;
static char g_prop_old[AI_PROP_CAP];

/* ---- the transcript model -------------------------------------------------- */

static int ai_room(void) { return AI_TEXT_CAP - 1 - g_len; }

static void ai_append(const char *s, int n)
{
    if (n > ai_room()) n = ai_room();
    if (n <= 0 || !g_nturn) return;
    memcpy(g_text + g_len, s, (unsigned long)n);
    g_len += n;
    g_text[g_len] = 0;
    g_turn[g_nturn - 1].len += n;
}

static void ai_turn(int role, const char *s, int n)
{
    if (g_nturn >= AI_TURNS) return;
    if (n < 0) n = (int)strlen(s);
    g_turn[g_nturn].start = g_len;
    g_turn[g_nturn].len = 0;
    g_turn[g_nturn].role = (unsigned char)role;
    g_nturn++;
    ai_append(s, n);
}

static void ai_note(const char *s) { ai_turn(AI_NOTE, s, -1); uc_repaint(); }

void uc_ai_clear(void)
{
    if (g_req) { uc_http_free(g_req); g_req = 0; }
    g_len = g_nturn = 0;
    g_text[0] = 0;
    g_scroll = 0;
    g_stick = 1;
    g_prop = -1;
    uc_repaint();
}

int uc_ai_busy(void) { return g_req != 0; }

/* ---- the exchange ---------------------------------------------------------- */

/* The SECOND slot (UCD-50): one exchange for a caller that brings its own
 * message list - the extension host's vscode.lm.  Same transport, same model
 * setting, same key; what differs is where the deltas go, and that both
 * callers can be in flight at once without knowing about each other. */
static uc_http    *g_lmreq;
static UcLmDeltaFn g_lm_delta;
static UcLmDoneFn  g_lm_done;
static void       *g_lm_user;
static char        g_lm_apierr[160];   /* an "error" SSE event, kept for done */

/* One SSE event.  The event NAME is advisory; the data's own "type" is what
 * the API documents, so that is what is switched on.  `user` says which slot
 * the stream belongs to: null is the chat transcript, anything else the LM
 * caller's. */
static void ai_sse(void *user, const char *event, const char *data, int len)
{
    UcJson *root;
    char err[80];
    const char *type;
    (void)event;
    root = uc_json_parse(data, len, err, sizeof err);
    if (!root) return;                          /* a ping, or noise - skip    */
    type = uc_json_str(root, "type", "");
    if (!strcmp(type, "content_block_delta")) {
        UcJson *t = uc_json_path(root, "delta.text");
        if (t && t->type == UJ_STR) {
            if (user) {
                if (g_lm_delta)
                    g_lm_delta(g_lm_user, t->str, (int)strlen(t->str));
            } else {
                int had = ai_room();
                ai_append(t->str, (int)strlen(t->str));
                if (had > 0 && ai_room() <= 0)
                    ai_note("The transcript is full - AI: New Chat to continue.");
                if (g_stick) g_scroll = 1 << 28;  /* clamp pins it to the end */
                uc_repaint();
            }
        }
    } else if (!strcmp(type, "error")) {
        UcJson *m = uc_json_path(root, "error.message");
        if (user) {
            uc_scpy(g_lm_apierr, m && m->type == UJ_STR ? m->str : "an error",
                    sizeof g_lm_apierr);
        } else {
            char msg[160];
            uc_scpy(msg, "The API reported: ", sizeof msg);
            uc_scat(msg, m && m->type == UJ_STR ? m->str : "an error",
                    sizeof msg);
            ai_note(msg);
        }
    }
    uc_json_free(root);
}

/* Start one streaming exchange with the configured model, `msgs` being the
 * JSON text of the "messages" array.  Both slots come through here, so there
 * is one place that knows the endpoint, the headers and where the key comes
 * from.  Returns the handle, or 0 with `why` set to a sentence. */
static uc_http *ai_begin_anthropic(const char *msgs, int msgs_len,
                                   const char *sys, int sys_len, void *user,
                                   const char **why)
{
    char key[UC_SECRET_MAX];
    char *b;
    int cap, p = 0;
    uc_http_req rq;
    uc_header hdr[4];
    uc_http *h;

    if (!uc_secret_get("anthropic.key", key, sizeof key)) {
        *why = "No API key is set - run \"AI: Set API Key\" from the command "
               "palette (Ctrl+Shift+P).";
        return 0;
    }
    /* x6 on the context, not x2: an escaped control byte becomes \u00XX, and
     * source files carry tabs. The p >= cap guard below still catches it. */
    cap = msgs_len + sys_len * 6 + 512;
    b = (char *)malloc((unsigned long)cap);
    if (!b) { *why = "out of memory building the request"; return 0; }
    uc_buf_raw(b, &p, cap, "{\"model\":");
    uc_buf_json(b, &p, cap, uc_cfg_str("ai.model"));
    uc_buf_raw(b, &p, cap, ",\"max_tokens\":");
    uc_buf_int(b, &p, cap, uc_cfg_int("ai.maxTokens"));
    /* The open file rides `system` rather than the transcript, so the
     * conversation the user can read back is the one they actually wrote. */
    if (sys && sys_len > 0) {
        uc_buf_raw(b, &p, cap, ",\"system\":");
        uc_buf_json(b, &p, cap, sys);
    }
    uc_buf_raw(b, &p, cap, ",\"stream\":true,\"messages\":");
    uc_buf_n(b, &p, cap, msgs, msgs_len);
    uc_buf_raw(b, &p, cap, "}");
    if (p >= cap) {
        free(b);
        *why = "the conversation is too large to send";
        return 0;
    }
    memset(&rq, 0, sizeof rq);
    rq.host = "api.anthropic.com";
    rq.method = "POST";
    rq.path = "/v1/messages";
    hdr[0].name = "x-api-key";         hdr[0].value = key;
    hdr[1].name = "anthropic-version"; hdr[1].value = "2023-06-01";
    hdr[2].name = "content-type";      hdr[2].value = "application/json";
    hdr[3].name = "accept";            hdr[3].value = "text/event-stream";
    rq.headers = hdr;
    rq.nheaders = 4;
    rq.body = b;
    rq.body_len = p;
    h = uc_http_begin(&rq);
    free(b);                        /* uc_http_begin copied what it needs     */
    memset(key, 0, sizeof key);
    if (!h) { *why = "the request could not be started"; return 0; }
    uc_http_on_event(h, ai_sse, user);
    return h;
}

/* The status-or-error sentence for a completed exchange.  Returns 0 when the
 * exchange was a clean 200. */
static const char *ai_finish_msg(uc_http *h, int poll_rc, char *out, int cap)
{
    int st;
    if (poll_rc < 0) { uc_scpy(out, uc_http_error(h), cap); return out; }
    st = uc_http_status(h);
    if (st == 200) return 0;
    {
        /* the error body survives streaming on purpose - see take_body() */
        int blen = 0;
        const char *body = uc_http_body(h, &blen);
        char num[16];
        UcJson *root = body ? uc_json_parse(body, blen, out, cap) : 0;
        UcJson *m = root ? uc_json_path(root, "error.message") : 0;
        uc_scpy(out, "HTTP ", cap);
        uc_itoa(num, st);
        uc_scat(out, num, cap);
        uc_scat(out, ": ", cap);
        uc_scat(out, m && m->type == UJ_STR ? m->str : "the request was refused",
                cap);
        if (st == 401)
            uc_scat(out, " - check the key with AI: Set API Key", cap);
        if (root) uc_json_free(root);
    }
    return out;
}

int uc_lm_begin(const char *messages_json, UcLmDeltaFn on_delta,
                UcLmDoneFn on_done, void *user, const char **why)
{
    if (g_lmreq) { *why = "one model request at a time - the running one has "
                          "not finished"; return 0; }
    g_lm_apierr[0] = 0;
    /* No file context here on purpose: an extension supplies its own messages
     * and its own framing, and quietly appending the user's open buffer to a
     * third-party extension's request would be exfiltration, not a feature. */
    g_lmreq = ai_begin_anthropic(messages_json, (int)strlen(messages_json),
                                 0, 0, &g_lmreq, why);
    if (!g_lmreq) return 0;
    g_lm_delta = on_delta;
    g_lm_done = on_done;
    g_lm_user = user;
    return 1;
}

void uc_lm_cancel(void)
{
    if (!g_lmreq) return;
    uc_http_free(g_lmreq);
    g_lmreq = 0;
    if (g_lm_done) g_lm_done(g_lm_user, 0, "cancelled");
}

static void ai_send(void)
{
    char *b;
    int cap, p = 0, i, first = 1;
    const char *why = 0;
    static char ctx[AI_CTX_CAP];       /* 24 KB - not a stack frame        */
    int ctx_len;

    if (g_req || !g_inlen) return;
    /* the key is checked BEFORE the turn is added, so a keyless send leaves
     * the question in the input rather than stranding it in the transcript */
    {
        char key[8];
        if (!uc_secret_get("anthropic.key", key, sizeof key)) {
            ai_note("No API key is set. Run \"AI: Set API Key\" from the "
                    "command palette (Ctrl+Shift+P).");
            return;
        }
    }
    ai_turn(AI_USER, g_input, g_inlen);
    g_inlen = 0;
    g_input[0] = 0;

    /* THE OPEN FILE GOES WITH THE QUESTION (UCD-58), and it is ANNOUNCED.
     *
     * Announced because a copy of the buffer leaving the machine is the one
     * thing here a user would want to know about, and until now the only
     * place it was written down was a warning box in the manual. A note names
     * the file on every send; notes are local, so the note itself is never
     * part of what is sent.
     *
     * Rebuilt per send rather than once per conversation: the file is being
     * EDITED, and answering turn three against the buffer as it stood at turn
     * one is a subtler wrong answer than refusing outright. */
    ctx_len = 0;
    ctx[0] = 0;
    {
        UcDoc *d = uc_doc_active();
        if (d && d->text && d->len > 0) {
            int trunc = 0;
            char note[UC_NAME_MAX + 64], num[24];
            const char *nm = d->name[0] ? d->name : "an untitled file";
            ctx_len = uc_ctx_file(ctx, (int)sizeof ctx, nm, d->text, d->len,
                                  &trunc);
            uc_scpy(note, "Sent with ", sizeof note);
            uc_scat(note, nm, sizeof note);
            uc_scat(note, " - ", sizeof note);
            uc_itoa(num, uc_line_count(d));
            uc_scat(note, num, sizeof note);
            uc_scat(note, trunc ? " lines, truncated to fit." : " lines.",
                    sizeof note);
            ai_note(note);
        }
    }

    /* the messages array: every user and assistant turn, in order, with
     * adjacent same-role turns MERGED - notes are dropped, and a dropped
     * note is how two user turns end up touching */
    cap = g_len * 2 + 1024;
    b = (char *)malloc((unsigned long)cap);
    if (!b) { ai_note("Out of memory building the request."); return; }
    uc_buf_raw(b, &p, cap, "[");
    for (i = 0; i < g_nturn; i++) {
        int role = g_turn[i].role;
        static char one[AI_TEXT_CAP];      /* 48 KB - not a stack frame      */
        int n = g_turn[i].len, k;
        if (role == AI_NOTE || n == 0) continue;
        if (first && role == AI_ASSIST) continue;     /* no leading assistant */
        memcpy(one, g_text + g_turn[i].start, (unsigned long)n);
        /* merge the adjacent same-role turns into this one */
        for (k = i + 1; k < g_nturn; k++) {
            if (g_turn[k].role == AI_NOTE || g_turn[k].len == 0) continue;
            if (g_turn[k].role != role) break;
            if (n + 2 + g_turn[k].len >= (int)sizeof one) break;
            one[n++] = '\n'; one[n++] = '\n';
            memcpy(one + n, g_text + g_turn[k].start, (unsigned long)g_turn[k].len);
            n += g_turn[k].len;
            i = k;
        }
        one[n] = 0;
        if (!first) uc_buf_raw(b, &p, cap, ",");
        uc_buf_raw(b, &p, cap, "{\"role\":");
        uc_buf_json(b, &p, cap, role == AI_USER ? "user" : "assistant");
        uc_buf_raw(b, &p, cap, ",\"content\":");
        uc_buf_json(b, &p, cap, one);
        uc_buf_raw(b, &p, cap, "}");
        first = 0;
    }
    uc_buf_raw(b, &p, cap, "]");
    if (p >= cap) {                 /* truncated JSON earns a 400, not a send */
        free(b);
        ai_note("The conversation is too large to send - AI: New Chat.");
        return;
    }
    b[p] = 0;
    g_req = ai_begin_anthropic(b, p, ctx_len ? ctx : 0, ctx_len, 0, &why);
    free(b);
    if (!g_req) { ai_note(why); return; }
    ai_turn(AI_ASSIST, "", 0);      /* the deltas stream into this turn      */
    g_stick = 1;
    uc_repaint();
}

void uc_ai_abort(void)
{
    if (!g_req) return;
    uc_http_free(g_req);
    g_req = 0;
    ai_note("Stopped.");
}

void uc_ai_tick(void)
{
    char msg[200];
    int r;
    if (!g_req && !g_lmreq) return;
    uc_net_pump();

    if (g_req && (r = uc_http_poll(g_req)) != UC_HTTP_PENDING) {
        /* a failed or empty exchange leaves no bare "Assistant" header */
        const char *err = ai_finish_msg(g_req, r, msg, sizeof msg);
        int was_empty = g_nturn && g_turn[g_nturn - 1].role == AI_ASSIST &&
                        g_turn[g_nturn - 1].len == 0;
        if (was_empty) g_nturn--;
        if (err) ai_note(err);
        else if (was_empty) ai_note("The reply was empty.");
        uc_http_free(g_req);
        g_req = 0;
        uc_repaint();
    }

    if (g_lmreq && (r = uc_http_poll(g_lmreq)) != UC_HTTP_PENDING) {
        const char *err = ai_finish_msg(g_lmreq, r, msg, sizeof msg);
        if (!err && g_lm_apierr[0]) err = g_lm_apierr;
        uc_http_free(g_lmreq);
        g_lmreq = 0;
        if (g_lm_done)
            g_lm_done(g_lm_user, err ? 0 : 200, err);
    }
}

/* ---- layout + drawing ------------------------------------------------------
 * One walker serves both: with a framebuffer clip in place it draws, and
 * either way it measures, records the code blocks and (when drawing) the
 * clickable regions.  Two copies of this arithmetic would disagree within a
 * week. */

static int ai_wrap_prose(int x, int y, int w, const char *s, int n, fb_px c,
                         int draw)
{
    int rh = uc_ui_h() + 3, i = 0;
    while (i < n) {
        int fit = 0, lastsp = -1, wpx = 0;
        while (i + fit < n && s[i + fit] != '\n') {
            /* widths accumulate per character; a word longer than the row
             * breaks mid-word rather than vanishing */
            char one[8];
            int cl = uc_u8_get(s + i + fit, n - i - fit, &(int){0});
            memcpy(one, s + i + fit, (unsigned long)cl);
            one[cl] = 0;
            wpx += uc_ui_text_w(one);
            if (wpx > w && fit > 0) break;
            if (s[i + fit] == ' ') lastsp = fit;
            fit += cl;
        }
        if (i + fit < n && s[i + fit] != '\n' && lastsp > 0) fit = lastsp;
        if (draw) {
            char row[256];
            int cn = fit < (int)sizeof row - 1 ? fit : (int)sizeof row - 1;
            memcpy(row, s + i, (unsigned long)cn);
            row[cn] = 0;
            uc_ui_text(x, y, row, c);
        }
        y += rh;
        i += fit;
        if (i < n && (s[i] == '\n' || s[i] == ' ')) i++;
    }
    return y;
}

static int ai_code_line(int x, int y, int w, const char *s, int n, int lang,
                        int *state, int draw)
{
    static short scopes[UC_HL_MAXLINE];
    int rh = uc_line_h(), out = *state;
    if (draw) {
        char line[240];
        int cn = n < (int)sizeof line - 1 ? n : (int)sizeof line - 1;
        int coloured, i;
        memcpy(line, s, (unsigned long)cn);
        line[cn] = 0;
        coloured = uc_tokenize(lang, line, cn, *state, scopes, &out);
        fb_set_clip(x, y, w, rh);
        if (!coloured)
            uc_mono(x, y, line, uc_col(UC_C_EDITOR_FG), 0);
        else {
            int cx = x;
            i = 0;
            while (i < cn) {
                int j = i, style = 0;
                fb_px c;
                while (j < cn && scopes[j] == scopes[i]) j++;
                c = scopes[i] ? uc_tok_color(uc_scope_name(scopes[i]), &style)
                              : uc_col(UC_C_EDITOR_FG);
                cx = uc_mono_n(cx, y, line + i, j - i, c, style);
                i = j;
            }
        }
        fb_reset_clip();
    } else
        uc_tokenize(lang, s, n < UC_HL_MAXLINE ? n : UC_HL_MAXLINE, *state,
                    scopes, &out);
    *state = out;
    return y + rh;
}

static void ai_hit_add(int x, int y, int w, int h, int kind, int arg)
{
    if (g_nhit >= AI_HITS) return;
    g_hit[g_nhit].r = (UcRect){ x, y, w, h };
    g_hit[g_nhit].kind = kind;
    g_hit[g_nhit].arg = arg;
    g_nhit++;
}

static int ai_button(int x, int y, const char *label, int kind, int arg,
                     int draw)
{
    int w = uc_ui_text_w(label) + 12, h = uc_ui_h() + 4;
    if (draw) {
        fb_fill_rect(x, y, w, h, uc_col(UC_C_BUTTON_BG));
        uc_ui_text(x + 6, y + 2, label, uc_col(UC_C_BUTTON_FG));
        ai_hit_add(x, y, w, h, kind, arg);
    }
    return w;
}

/* the proposal panel, drawn under the block it belongs to */
static int ai_proposal(int x, int y, int w, int draw)
{
    int rh = uc_line_h(), i, n;
    const char *olds = g_prop_old;
    const char *news = g_text + g_blk[g_prop].start;
    int newn = g_blk[g_prop].len;
    if (draw) uc_ui_text(x, y, "Proposed edit:", uc_col(UC_C_SIDEBAR_TITLE));
    y += uc_ui_h() + 4;
    n = (int)strlen(olds);
    i = 0;
    while (i < n) {                                 /* the lines leaving      */
        int e = i;
        while (e < n && olds[e] != '\n') e++;
        if (draw) {
            fb_blend_rect(x, y, w, rh, uc_col(UC_C_GUTTER_DELETED), 46);
            uc_mono(x + 2, y, "-", uc_col(UC_C_GIT_DELETED), 0);
            fb_set_clip(x + 12, y, w - 12, rh);
            {
                char row[240];
                int cn = e - i < 239 ? e - i : 239;
                memcpy(row, olds + i, (unsigned long)cn);
                row[cn] = 0;
                uc_mono(x + 12, y, row, uc_col(UC_C_EDITOR_FG), 0);
            }
            fb_reset_clip();
        }
        y += rh;
        i = e + 1;
    }
    i = 0;
    while (i < newn) {                              /* the lines arriving     */
        int e = i;
        while (e < newn && news[e] != '\n') e++;
        if (draw) {
            fb_blend_rect(x, y, w, rh, uc_col(UC_C_GUTTER_ADDED), 46);
            uc_mono(x + 2, y, "+", uc_col(UC_C_GIT_ADDED), 0);
            fb_set_clip(x + 12, y, w - 12, rh);
            {
                char row[240];
                int cn = e - i < 239 ? e - i : 239;
                memcpy(row, news + i, (unsigned long)cn);
                row[cn] = 0;
                uc_mono(x + 12, y, row, uc_col(UC_C_EDITOR_FG), 0);
            }
            fb_reset_clip();
        }
        y += rh;
        i = e + 1;
    }
    y += 4;
    if (draw) {
        int bx = x;
        bx += ai_button(bx, y, "Apply", HIT_PROP_APPLY, 0, 1) + 6;
        ai_button(bx, y, "Reject", HIT_PROP_REJECT, 0, 1);
    }
    return y + uc_ui_h() + 8;
}

/* Walk the whole transcript.  `draw` also records hit regions; either way the
 * code-block table is rebuilt and the total height returned. */
static int ai_walk(UcRect r, int draw)
{
    int y = r.y - g_scroll, i, rh = uc_ui_h() + 3;
    int pad = 8, w = r.w - pad * 2, x = r.x + pad;
    if (draw) g_nhit = 0;
    g_nblk = 0;

    /* header: the model, and a fresh start */
    if (draw) {
        uc_ui_text(x, y + 2, uc_cfg_str("ai.model"), uc_col(UC_C_BREADCRUMB_FG));
        ai_button(x + w - uc_ui_text_w("New chat") - 12, y, "New chat",
                  HIT_NEW, 0, 1);
    }
    y += rh + 6;

    for (i = 0; i < g_nturn; i++) {
        const char *s = g_text + g_turn[i].start;
        int n = g_turn[i].len, role = g_turn[i].role, k = 0;
        fb_px rc = role == AI_USER ? uc_col(UC_C_LIST_HIGHLIGHT)
                 : role == AI_NOTE ? uc_col(UC_C_WARN_FG)
                                   : uc_col(UC_C_INFO_FG);
        if (draw)
            uc_ui_text(x, y, role == AI_USER ? "You"
                            : role == AI_NOTE ? "Note" : "Assistant", rc);
        y += rh;
        while (k < n) {
            /* a fence line opens a code block */
            if (n - k >= 3 && !strncmp(s + k, "```", 3)) {
                char lang[14];
                int e = k + 3, li = 0, cs = k, ce, lidx, state = 0, bi;
                while (e < n && s[e] != '\n') {
                    if (li < (int)sizeof lang - 1) lang[li++] = s[e];
                    e++;
                }
                lang[li] = 0;
                cs = (e < n) ? e + 1 : n;            /* content start        */
                ce = cs;
                while (ce < n && !(n - ce >= 3 && !strncmp(s + ce, "```", 3) &&
                                   (ce == 0 || s[ce - 1] == '\n')))
                    ce++;
                bi = g_nblk;
                if (g_nblk < AI_BLOCKS) {
                    g_blk[g_nblk].turn = i;
                    g_blk[g_nblk].start = (int)(s - g_text) + cs;
                    g_blk[g_nblk].len = ce - cs;
                    uc_scpy(g_blk[g_nblk].lang, lang, sizeof g_blk[0].lang);
                    g_nblk++;
                }
                lidx = uc_lang_by_id(lang);
                if (lidx < 0) lidx = 0;
                /* the block header: language + Apply */
                if (draw) {
                    fb_fill_rect(x, y, w, rh, uc_col(UC_C_SIDEBAR_SECT));
                    uc_ui_text(x + 4, y + 1, lang[0] ? lang : "code",
                               uc_col(UC_C_SIDEBAR_FG));
                    ai_button(x + w - uc_ui_text_w("Apply") - 12, y - 1,
                              "Apply", HIT_APPLY, bi, 1);
                }
                y += rh;
                if (draw) {
                    int bh = 0, cc = cs;
                    while (cc < ce) {               /* block height first     */
                        int le = cc;
                        while (le < ce && s[le] != '\n') le++;
                        bh += uc_line_h();
                        cc = le + 1;
                    }
                    fb_fill_rect(x, y, w, bh + 4, uc_col(UC_C_TERM_BG));
                }
                {
                    int cc = cs;
                    y += 2;
                    while (cc < ce) {
                        int le = cc;
                        while (le < ce && s[le] != '\n') le++;
                        y = ai_code_line(x + 4, y, w - 8,
                                         s + cc, le - cc, lidx, &state, draw);
                        cc = le + 1;
                    }
                }
                y += 4;
                if (g_prop == bi && bi < g_nblk)
                    y = ai_proposal(x, y + 2, w, draw);
                k = ce;
                if (k < n) {                         /* skip the closing fence */
                    while (k < n && s[k] != '\n') k++;
                    if (k < n) k++;
                }
                continue;
            }
            /* prose up to the next fence */
            {
                int e = k;
                while (e < n && !(n - e >= 3 && !strncmp(s + e, "```", 3) &&
                                  (e == k || s[e - 1] == '\n')))
                    e++;
                y = ai_wrap_prose(x, y, w, s + k, e - k,
                                  role == AI_NOTE ? uc_col(UC_C_WARN_FG)
                                                  : uc_col(UC_C_SIDEBAR_FG),
                                  draw);
                k = e;
            }
        }
        y += 6;
    }
    if (g_req && draw) {
        uc_ui_text(x, y, "generating... (Esc stops it)",
                   uc_col(UC_C_BREADCRUMB_FG));
    }
    if (g_req) y += rh;
    return y + g_scroll - r.y;                       /* total content height  */
}

void uc_ai_draw(UcRect r)
{
    int rh = uc_ui_h() + 3, in_h, i;
    UcRect body, inbox;
    /* the input grows with its wrapped text, up to four rows */
    {
        int rows = 1, wpx = 0, w = r.w - 28;
        for (i = 0; i < g_inlen; i++) {
            char one[2] = { g_input[i], 0 };
            if (g_input[i] == '\n') { rows++; wpx = 0; continue; }
            wpx += uc_ui_text_w(one);
            if (wpx > w) { rows++; wpx = 0; }
        }
        if (rows > 4) rows = 4;
        in_h = rows * rh + 10;
    }
    body = (UcRect){ r.x, r.y, r.w, r.h - in_h - 6 };
    inbox = (UcRect){ r.x + 6, r.y + r.h - in_h - 2, r.w - 12, in_h };
    g_view_h = body.h;

    /* clamp the scroll against the fresh height, pinning while streaming */
    g_content_h = ai_walk(body, 0);
    if (g_scroll > g_content_h - body.h) g_scroll = g_content_h - body.h;
    if (g_scroll < 0) g_scroll = 0;

    fb_set_clip(body.x, body.y, body.w, body.h);
    ai_walk(body, 1);
    fb_reset_clip();

    /* the input box */
    fb_fill_rect(inbox.x, inbox.y, inbox.w, inbox.h, uc_col(UC_C_INPUT_BG));
    fb_frame_rect(inbox.x, inbox.y, inbox.w, inbox.h,
                  UC.focus == UC_F_SIDEBAR ? uc_col(UC_C_FOCUS_BORDER)
                                           : uc_col(UC_C_INPUT_BORDER));
    if (g_inlen) {
        fb_set_clip(inbox.x + 1, inbox.y + 1, inbox.w - 2, inbox.h - 2);
        ai_wrap_prose(inbox.x + 6, inbox.y + 5, inbox.w - 12, g_input, g_inlen,
                      uc_col(UC_C_INPUT_FG), 1);
        fb_reset_clip();
    } else
        uc_ui_text(inbox.x + 6, inbox.y + 5,
                   g_req ? "waiting for the reply..." : "Ask about your code",
                   uc_col(UC_C_INPUT_PLACEHOLDER));
}

/* ---- applying a block ------------------------------------------------------ */

static void ai_apply_now(void)
{
    UcDoc *d = uc_doc_active();
    const char *src;
    int n;
    if (g_prop < 0 || g_prop >= g_nblk) { g_prop = -1; return; }
    if (!d) { ai_note("No editor is open to apply into."); g_prop = -1; return; }
    src = g_text + g_blk[g_prop].start;
    n = g_blk[g_prop].len;
    uc_begin_group(d);
    if (uc_has_selection(d)) uc_delete_selection(d);
    uc_insert(d, src, n);
    uc_end_group(d);
    g_prop = -1;
    uc_focus(UC_F_EDITOR);
    uc_repaint();
}

/* ---- events ---------------------------------------------------------------- */

int uc_ai_event(UcRect r, const unoui_event *e)
{
    int i;
    if (e->kind == UI_EV_WHEEL) {
        g_scroll += e->wheel * (uc_ui_h() + 3) * 3;
        g_stick = g_scroll >= g_content_h - g_view_h - 4;
        uc_repaint();
        return 1;
    }
    if (e->kind != UI_EV_MOUSE_DOWN) return 0;
    (void)r;
    for (i = 0; i < g_nhit; i++) {
        UcRect *h = &g_hit[i].r;
        if (e->x < h->x || e->x >= h->x + h->w ||
            e->y < h->y || e->y >= h->y + h->h) continue;
        switch (g_hit[i].kind) {
        case HIT_NEW:
            uc_ai_clear();
            return 1;
        case HIT_APPLY: {
            UcDoc *d = uc_doc_active();
            g_prop = g_hit[i].arg;
            g_prop_old[0] = 0;
            if (d && uc_has_selection(d))
                uc_selection_text(d, g_prop_old, sizeof g_prop_old);
            uc_repaint();
            return 1;
        }
        case HIT_PROP_APPLY:
            ai_apply_now();
            return 1;
        case HIT_PROP_REJECT:
            g_prop = -1;
            uc_repaint();
            return 1;
        }
    }
    return 1;
}

int uc_ai_key(int key, int mods, int ch)
{
    if (key == UI_KEY_ESC) {
        if (g_req) { uc_ai_abort(); return 1; }
        if (g_prop >= 0) { g_prop = -1; uc_repaint(); return 1; }
        return 0;
    }
    if (key == UI_KEY_ENTER) {
        /* Shift+Enter is a newline, Enter sends - a question with a code
         * fence in it needs line breaks, and this is the shape VS Code's own
         * chat taught everyone's fingers */
        if (mods & UI_MOD_SHIFT) {
            if (g_inlen < AI_INPUT_CAP - 2) {
                g_input[g_inlen++] = '\n';
                g_input[g_inlen] = 0;
                uc_repaint();
            }
            return 1;
        }
        ai_send();
        return 1;
    }
    if (key == UI_KEY_BACKSPACE) {
        if (g_inlen > 0) {
            g_inlen = uc_u8_back(g_input, g_inlen);
            g_input[g_inlen] = 0;
            uc_repaint();
        }
        return 1;
    }
    if (key == UI_KEY_PGUP) { g_scroll -= g_view_h; g_stick = 0; uc_repaint(); return 1; }
    if (key == UI_KEY_PGDN) { g_scroll += g_view_h; uc_repaint(); return 1; }
    if (ch >= 32) {
        int cl = uc_u8_len(ch);
        if (g_inlen + cl < AI_INPUT_CAP - 1) {
            g_inlen += uc_u8_put(ch, g_input + g_inlen);
            g_input[g_inlen] = 0;
            uc_repaint();
        }
        return 1;
    }
    return 0;
}

void uc_ai_open(void)
{
    uc_toggle_sidebar(UC_VIEW_ASSIST);
    if (UC.view == UC_VIEW_ASSIST && UC.sidebar_visible) uc_focus(UC_F_SIDEBAR);
}
