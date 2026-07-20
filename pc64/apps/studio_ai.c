/* ===========================================================================
 * Studio - the AI assistant pane.
 *
 * A self-contained HTTPS chat client for OpenAI, Anthropic and Gemini, built
 * entirely from kernel exports: pc64_net_up + net_dns_query + tls_connect_ca
 * bring up a CA-validated TLS session, the module writes the HTTP/1.1 POST
 * and reads the reply, and studio_json.c pulls the assistant's text out of
 * the response.  No new kernel code - the whole client is module-side.
 *
 * Keys live in AI.CFG (plaintext on the disk - a stated caveat), set with
 * slash commands in the input line (/provider /model /key /save).  The
 * request is issued in one frame after a "thinking..." repaint, so the desk
 * shows progress; a big response briefly stalls the UI, which v1 accepts.
 * ======================================================================== */
#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "studio_ai.h"
#include "studio_json.h"

/* ---- imports -------------------------------------------------------------- */
typedef unsigned char u8;
void  fb_fill_rect(int x, int y, int w, int h, fb_px c);
void  fb_hline(int x, int y, int w, fb_px c);
void  fb_vline(int x, int y, int h, fb_px c);
int   fb_text(int x, int y, const char *s, fb_px fg, long bg);
int   fb_text_w(const char *s);
int   fb_text_h(void);
void  pc64_shell_dirty(void);

int   pc64_net_up(void);
int   net_dns_query(const char *host, u8 out[4]);
int   tls_connect_ca(const u8 dst[4], unsigned short port, const char *sni);
int   tls_write(const void *data, int len);
int   tls_read(void *buf, int cap);
void  tls_close(void);
int   tls_have_rdrand(void);

int   uno_fs_volumes(void);
int   uno_fs_writable(int vol);
long  uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
int   uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);

void *malloc(unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
unsigned long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);

/* ---- providers ------------------------------------------------------------ */
enum { PV_OPENAI, PV_ANTHROPIC, PV_GEMINI, PV_N };
static const struct {
    const char *name, *host, *path_pre, *path_post, *model, *reply;
} kProv[PV_N] = {
    { "openai", "api.openai.com", "/v1/chat/completions", "",
      "gpt-4o-mini", "choices.0.message.content" },
    { "anthropic", "api.anthropic.com", "/v1/messages", "",
      "claude-sonnet-4-5", "content.0.text" },
    { "gemini", "generativelanguage.googleapis.com",
      "/v1beta/models/", ":generateContent",
      "gemini-2.0-flash", "candidates.0.content.parts.0.text" },
};

static int  cfg_provider = PV_ANTHROPIC;
static char cfg_model[48];
static char cfg_key[PV_N][160];
static char cfg_host[48];        /* dev IP[:port] hint; SNI + cert stay real */
static int  cfg_vol = -1;

/* ---- conversation --------------------------------------------------------- */
#define MSG_MAX   96
#define CONV_CAP  (48 * 1024)
enum { ROLE_USER, ROLE_AI, ROLE_SYS };
static char *conv;
static int   conv_len;
static struct { int role, off, len; } msg[MSG_MAX];
static int   nmsg;
static int   ai_scroll;

static char *req_buf, *resp_buf, *reply_buf, *att_file;
#define REQ_CAP   (96 * 1024)
#define RESP_CAP  (160 * 1024)
#define REPLY_CAP (48 * 1024)

static char in_line[256]; static int in_len;
static int  g_focused, req_state, busy;
static char att_name[16]; static int att_len, att_pending;

/* ---- utils ---------------------------------------------------------------- */
static void s_cpy(char *d, const char *s, int cap)
{ int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int  s_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static void conv_add(int role, const char *s, int n)
{
    if (nmsg >= MSG_MAX) {                          /* drop the oldest turn */
        int base = msg[0].off + msg[0].len, i;
        memcpy(conv, conv + base, (unsigned long)(conv_len - base));
        conv_len -= base;
        for (i = 1; i < nmsg; i++) { msg[i-1] = msg[i]; msg[i-1].off -= base; }
        nmsg--;
    }
    if (n > CONV_CAP - conv_len - 1) n = CONV_CAP - conv_len - 1;
    if (n < 0) n = 0;
    msg[nmsg].role = role; msg[nmsg].off = conv_len; msg[nmsg].len = n;
    memcpy(conv + conv_len, s, (unsigned long)n);
    conv_len += n; conv[conv_len] = 0;
    nmsg++;
    ai_scroll = 1 << 20;                            /* pin to the bottom */
}
static void conv_addz(int role, const char *s) { conv_add(role, s, (int)strlen(s)); }

/* ---- config file ---------------------------------------------------------- */
static void cfg_load(void)
{
    int nv = uno_fs_volumes(), v;
    unsigned char buf[2048];
    long n = -1;
    s_cpy(cfg_model, kProv[cfg_provider].model, sizeof cfg_model);
    for (v = 0; v < nv; v++) {
        n = uno_fs_read(v, "AI.CFG", buf, sizeof buf - 1);
        if (n < 0) n = uno_fs_read(v, "EFI\\UNODOS\\AI.CFG", buf, sizeof buf - 1);
        if (n >= 0) { cfg_vol = v; break; }
    }
    if (cfg_vol < 0) for (v = 0; v < nv; v++) if (uno_fs_writable(v)) { cfg_vol = v; break; }
    if (n < 0) return;
    buf[n] = 0;
    { char *p = (char *)buf, line[256];
      while (*p) {
        int i = 0;
        while (*p && *p != '\n' && i < 254) { if (*p != '\r') line[i++] = *p; p++; }
        while (*p == '\n' || *p == '\r') p++;
        line[i] = 0;
        { char *eq = line; while (*eq && *eq != '=') eq++;
          if (*eq == '=') { *eq = 0; { const char *k = line, *val = eq + 1;
            if (s_eq(k, "provider")) { int j; for (j = 0; j < PV_N; j++) if (s_eq(val, kProv[j].name)) cfg_provider = j; }
            else if (s_eq(k, "model")) s_cpy(cfg_model, val, sizeof cfg_model);
            else if (s_eq(k, "key_openai")) s_cpy(cfg_key[PV_OPENAI], val, 160);
            else if (s_eq(k, "key_anthropic")) s_cpy(cfg_key[PV_ANTHROPIC], val, 160);
            else if (s_eq(k, "key_gemini")) s_cpy(cfg_key[PV_GEMINI], val, 160);
            else if (s_eq(k, "host")) s_cpy(cfg_host, val, sizeof cfg_host);
            /* no "insecure" key: cert validation is never disableable from disk */
          } } }
      }
    }
}

static void cfg_save(void)
{
    char out[1024]; int p = 0;
    #define PUT(s) do { const char *q=(s); while(*q && p<1020) out[p++]=*q++; } while(0)
    PUT("provider="); PUT(kProv[cfg_provider].name); PUT("\n");
    PUT("model="); PUT(cfg_model); PUT("\n");
    PUT("key_openai="); PUT(cfg_key[PV_OPENAI]); PUT("\n");
    PUT("key_anthropic="); PUT(cfg_key[PV_ANTHROPIC]); PUT("\n");
    PUT("key_gemini="); PUT(cfg_key[PV_GEMINI]); PUT("\n");
    if (cfg_host[0]) { PUT("host="); PUT(cfg_host); PUT("\n"); }
    #undef PUT
    out[p] = 0;
    if (cfg_vol >= 0 && uno_fs_write(cfg_vol, "AI.CFG", (unsigned char *)out, p))
        conv_addz(ROLE_SYS, "Saved AI.CFG.");
    else
        conv_addz(ROLE_SYS, "Could not write AI.CFG (no writable volume?).");
}

/* ---- the system prompt ---------------------------------------------------- */
static const char *SYS_PROMPT =
    "You are the assistant inside UnoDOS Studio, the IDE of UnoDOS pc64. "
    "Apps are written in UnoC or in Python - both are first-class.\n"
    "UnoC is a C subset: char/short/int/long (long is 4 bytes), pointers, "
    "arrays, structs/unions, enums, typedef, function pointers; no floats, "
    "varargs, or function-like macros. A UnoC app includes \"UNO.H\", defines "
    "const AppInterface *uno_app_main(const KernelApi *k) returning a "
    "draw/key/click/tick vtable, and draws through the KernelApi.\n"
    "Python apps `import uno` and define a class subclassing uno.App with a "
    "module-global `app = MyApp()`. Methods: build(self, cv) (setup), "
    "draw(self, cv) (paint a frame), tick(self) (~60 Hz update), "
    "key(self, uni, scan, ctrl)->bool. `cv` is a Canvas: clear(color), "
    "fill_rect(x,y,w,h,color), rect(...), pixel(x,y,color), hline/vline, "
    "text(x,y,str,color), width(), height(). uno.rgb(r,g,b) makes a colour; "
    "uno.beep(midi,ticks), uno.read/write/size for files. Full float math is "
    "available in Python. Pick the language the user asks for (default Python "
    "for beginners, UnoC when they want native speed). Keep answers short and "
    "give runnable code. Fence code in triple backticks.";

/* ---- request body --------------------------------------------------------- */
static void build_request(const char *usermsg, char *body, int cap, int *blen)
{
    int p = 0;
    const char *M = cfg_model[0] ? cfg_model : kProv[cfg_provider].model;
    if (cfg_provider == PV_OPENAI) {
        jz_raw(body, &p, cap, "{\"model\":");
        jz_str(body, &p, cap, M);
        jz_raw(body, &p, cap, ",\"max_tokens\":1024,\"messages\":[{\"role\":\"system\",\"content\":");
        jz_str(body, &p, cap, SYS_PROMPT);
        jz_raw(body, &p, cap, "},{\"role\":\"user\",\"content\":");
        jz_str(body, &p, cap, usermsg);
        jz_raw(body, &p, cap, "}]}");
    } else if (cfg_provider == PV_ANTHROPIC) {
        jz_raw(body, &p, cap, "{\"model\":");
        jz_str(body, &p, cap, M);
        jz_raw(body, &p, cap, ",\"max_tokens\":1024,\"system\":");
        jz_str(body, &p, cap, SYS_PROMPT);
        jz_raw(body, &p, cap, ",\"messages\":[{\"role\":\"user\",\"content\":");
        jz_str(body, &p, cap, usermsg);
        jz_raw(body, &p, cap, "}]}");
    } else {
        jz_raw(body, &p, cap, "{\"systemInstruction\":{\"parts\":[{\"text\":");
        jz_str(body, &p, cap, SYS_PROMPT);
        jz_raw(body, &p, cap, "}]},\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":");
        jz_str(body, &p, cap, usermsg);
        jz_raw(body, &p, cap, "}]}]}");
    }
    *blen = p;
}

static void hdr(char *r, int *p, int cap, const char *name, const char *val)
{ jz_raw(r, p, cap, name); jz_raw(r, p, cap, ": "); jz_raw(r, p, cap, val); jz_raw(r, p, cap, "\r\n"); }

/* split (and de-chunk) an HTTP response in place; returns body + *blen */
static char *http_body(char *raw, int rawlen, int *blen)
{
    char *b = 0, *e = raw + rawlen, *p, *h;
    int chunked = 0;
    for (p = raw; p + 3 < e; p++)
        if (p[0]=='\r'&&p[1]=='\n'&&p[2]=='\r'&&p[3]=='\n') { b = p + 4; break; }
    if (!b) { *blen = 0; return raw; }
    for (h = raw; h + 7 < b; h++)
        if ((h[0]=='c'||h[0]=='C') && h[1]=='h' && !strncmp(h+1, "hunked", 6)) { chunked = 1; break; }
    if (!chunked) { *blen = (int)(e - b); return b; }
    { char *out = b, *in = b; int total = 0;
      while (in < e) {
        int sz = 0;
        while (in < e && *in != '\r' && *in != '\n') {
            int d = *in;
            if (d >= '0' && d <= '9') d -= '0';
            else if (d >= 'a' && d <= 'f') d -= 'a' - 10;
            else if (d >= 'A' && d <= 'F') d -= 'A' - 10;
            else break;
            sz = sz * 16 + d; in++;
        }
        while (in < e && (*in == '\r' || *in == '\n')) in++;
        if (sz <= 0) break;
        if (in + sz > e) sz = (int)(e - in);
        memcpy(out, in, (unsigned long)sz);
        out += sz; in += sz; total += sz;
        while (in < e && (*in == '\r' || *in == '\n')) in++;
      }
      *blen = total; return b;
    }
}

/* parse "a.b.c.d[:port]" test override into ip[4] + *port */
static void parse_hostport(const char *s, u8 ip[4], int *port)
{
    int i = 0, dots = 0, val = 0;
    for (; s[i] && s[i] != ':'; i++) {
        if (s[i] == '.') { ip[dots++] = (u8)val; val = 0; }
        else val = val * 10 + (s[i] - '0');
    }
    if (dots < 4) ip[dots] = (u8)val;
    if (s[i] == ':') { *port = 0; i++; while (s[i]) *port = *port * 10 + (s[i++] - '0'); }
}

/* ---- the request (blocking; called once from frame after "thinking") ------ */
static int do_request(const char *usermsg)
{
    u8 ip[4];
    int port = 443, blen, hp = 0, n, rawlen = 0, bodylen;
    char host[64];
    char *body;

    /* every scratch buffer is used unconditionally below; if any malloc in
     * studio_ai_init failed, bail instead of writing through a null pointer. */
    if (!conv || !req_buf || !resp_buf || !reply_buf) {
        if (conv) conv_addz(ROLE_SYS, "Out of memory (assistant buffers unavailable).");
        return -1; }

    if (!cfg_key[cfg_provider][0] && !cfg_host[0]) {
        conv_addz(ROLE_SYS, "No API key. Set one: /key <your-key> then /save"); return -1; }
    if (!pc64_net_up()) { conv_addz(ROLE_SYS, "No network link (need a wired NIC)."); return -1; }
    if (!tls_have_rdrand())
        conv_addz(ROLE_SYS, "Warning: no RDRAND - TLS entropy is weak here.");

    /* host is always the real provider name: it is the TLS SNI and the name the
     * cert is validated against.  cfg_host only redirects the destination IP/port
     * (a dev hint), so even a redirected connection must still present a cert
     * valid for the real provider - a rogue IP cannot pass tls_connect_ca. */
    s_cpy(host, kProv[cfg_provider].host, sizeof host);
    if (cfg_host[0]) parse_hostport(cfg_host, ip, &port);
    else if (!net_dns_query(host, ip)) { conv_addz(ROLE_SYS, "DNS lookup failed."); return -1; }

    if (tls_connect_ca(ip, (unsigned short)port, host) != 0)
        { conv_addz(ROLE_SYS, "TLS connect failed (cert not trusted / clock wrong)."); return -1; }

    build_request(usermsg, resp_buf, RESP_CAP, &blen);   /* body -> resp scratch */

    {
        char clen[16]; int c = blen, ci = 0; char tmp[16]; int tt = 0;
        if (!c) tmp[tt++] = '0';
        while (c) { tmp[tt++] = (char)('0' + c % 10); c /= 10; }
        while (tt) clen[ci++] = tmp[--tt]; clen[ci] = 0;

        jz_raw(req_buf, &hp, REQ_CAP, "POST ");
        jz_raw(req_buf, &hp, REQ_CAP, kProv[cfg_provider].path_pre);
        if (cfg_provider == PV_GEMINI) {
            jz_raw(req_buf, &hp, REQ_CAP, cfg_model[0] ? cfg_model : kProv[cfg_provider].model);
            jz_raw(req_buf, &hp, REQ_CAP, kProv[cfg_provider].path_post);
        }
        jz_raw(req_buf, &hp, REQ_CAP, " HTTP/1.1\r\n");
        hdr(req_buf, &hp, REQ_CAP, "Host", host);
        hdr(req_buf, &hp, REQ_CAP, "User-Agent", "UnoDOS-Studio");
        hdr(req_buf, &hp, REQ_CAP, "Content-Type", "application/json");
        hdr(req_buf, &hp, REQ_CAP, "Content-Length", clen);
        hdr(req_buf, &hp, REQ_CAP, "Connection", "close");
        if (cfg_provider == PV_OPENAI) {
            char bearer[176]; int i = 0;
            s_cpy(bearer, "Bearer ", sizeof bearer);
            while (cfg_key[PV_OPENAI][i] && i < 167) { bearer[7+i] = cfg_key[PV_OPENAI][i]; i++; }
            bearer[7+i] = 0;
            hdr(req_buf, &hp, REQ_CAP, "Authorization", bearer);
        } else if (cfg_provider == PV_ANTHROPIC) {
            hdr(req_buf, &hp, REQ_CAP, "x-api-key", cfg_key[PV_ANTHROPIC]);
            hdr(req_buf, &hp, REQ_CAP, "anthropic-version", "2023-06-01");
        } else {
            hdr(req_buf, &hp, REQ_CAP, "x-goog-api-key", cfg_key[PV_GEMINI]);
        }
        jz_raw(req_buf, &hp, REQ_CAP, "\r\n");
        { int i; for (i = 0; i < blen && hp < REQ_CAP - 1; i++) req_buf[hp++] = resp_buf[i]; req_buf[hp] = 0; }
    }

    if (tls_write(req_buf, hp) < 0) { conv_addz(ROLE_SYS, "TLS write failed."); tls_close(); return -1; }

    while (rawlen < RESP_CAP - 1) {
        char tmp[2048];
        n = tls_read(tmp, sizeof tmp);
        if (n > 0) { if (rawlen + n > RESP_CAP - 1) n = RESP_CAP - 1 - rawlen;
                     memcpy(resp_buf + rawlen, tmp, (unsigned long)n); rawlen += n; }
        else break;
    }
    resp_buf[rawlen] = 0;
    tls_close();

    body = http_body(resp_buf, rawlen, &bodylen);
    body[bodylen] = 0;

    if (jz_get_string(body, kProv[cfg_provider].reply, reply_buf, REPLY_CAP) >= 0)
        { conv_addz(ROLE_AI, reply_buf); return 0; }
    if (jz_get_string(body, "error.message", reply_buf, REPLY_CAP) >= 0)
        { conv_addz(ROLE_SYS, reply_buf); return -1; }
    { char st[64]; int i = 0; while (resp_buf[i] && resp_buf[i] != '\r' && i < 62) { st[i] = resp_buf[i]; i++; } st[i] = 0;
      conv_addz(ROLE_SYS, st[0] ? st : "Empty response."); }
    return -1;
}

/* ---- input ---------------------------------------------------------------- */
static void submit(void)
{
    if (!in_len) return;
    if (in_line[0] == '/') {
        char *a = in_line + 1, *sp = a;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') *sp++ = 0;
        if (s_eq(a, "provider")) { int j; for (j = 0; j < PV_N; j++) if (s_eq(sp, kProv[j].name)) { cfg_provider = j; s_cpy(cfg_model, kProv[j].model, sizeof cfg_model); } conv_addz(ROLE_SYS, kProv[cfg_provider].name); }
        else if (s_eq(a, "model")) { s_cpy(cfg_model, sp, sizeof cfg_model); conv_addz(ROLE_SYS, cfg_model); }
        else if (s_eq(a, "key"))   { s_cpy(cfg_key[cfg_provider], sp, 160); conv_addz(ROLE_SYS, "Key set (save with /save)."); }
        else if (s_eq(a, "host"))  { s_cpy(cfg_host, sp, sizeof cfg_host); conv_addz(ROLE_SYS, "host override set (SNI/cert stay real)."); }
        else if (s_eq(a, "save"))  cfg_save();
        else if (s_eq(a, "clear")) { nmsg = 0; conv_len = 0; }
        else conv_addz(ROLE_SYS, "commands: /provider /model /key /host /save /clear");
        in_len = 0; in_line[0] = 0;
        return;
    }
    conv_add(ROLE_USER, in_line, in_len);
    req_state = 1; busy = 1;
    in_len = 0; in_line[0] = 0;
}

static char g_send[80 * 1024];
static void make_send_text(void)
{
    int p = 0, i;
    const char *last = conv + msg[nmsg-1].off;
    int lastn = msg[nmsg-1].len;
    for (i = 0; i < lastn && p < (int)sizeof g_send - 1; i++) g_send[p++] = last[i];
    if (att_pending && att_file) {
        const char *h = "\n\n--- attached file: ";
        int j = 0; while (h[j] && p < (int)sizeof g_send - 1) g_send[p++] = h[j++];
        j = 0; while (att_name[j] && p < (int)sizeof g_send - 1) g_send[p++] = att_name[j++];
        if (p < (int)sizeof g_send - 2) { g_send[p++] = '\n'; }
        { int cap = (int)sizeof g_send - 1 - p, cn = att_len > cap ? cap : att_len;
          memcpy(g_send + p, att_file, (unsigned long)cn); p += cn; }
        att_pending = 0;
    }
    g_send[p] = 0;
}

/* ---- public API ----------------------------------------------------------- */
void studio_ai_init(void)
{
    if (conv) return;
    conv      = (char *)malloc(CONV_CAP);
    req_buf   = (char *)malloc(REQ_CAP);
    resp_buf  = (char *)malloc(RESP_CAP);
    reply_buf = (char *)malloc(REPLY_CAP);
    att_file  = (char *)malloc(64 * 1024);
    if (conv) conv[0] = 0;
    cfg_load();
    conv_addz(ROLE_SYS, "Assistant ready. Type a question, or /help. "
                        "Set a key with /key <value> then /save.");
}

void studio_ai_frame(void)
{
    if (req_state == 1) { req_state = 2; pc64_shell_dirty(); return; }
    if (req_state == 2) { make_send_text(); do_request(g_send); req_state = 0; busy = 0; pc64_shell_dirty(); }
}

int studio_ai_char(int ch)
{
    if (!g_focused || busy) return g_focused;
    if (ch >= 32 && ch < 127 && in_len < (int)sizeof in_line - 1)
        { in_line[in_len++] = (char)ch; in_line[in_len] = 0; }
    return 1;
}

int studio_ai_key(int vk)
{
    if (!g_focused) return 0;
    if (busy) return 1;
    if (vk == UI_KEY_ENTER)     { submit(); return 1; }
    if (vk == UI_KEY_BACKSPACE) { if (in_len) in_line[--in_len] = 0; return 1; }
    if (vk == UI_KEY_UP)   { ai_scroll -= fb_text_h() * 3; if (ai_scroll < 0) ai_scroll = 0; return 1; }
    if (vk == UI_KEY_DOWN) { ai_scroll += fb_text_h() * 3; return 1; }
    return 1;
}

int  studio_ai_accel(int uni, int ctrl) { (void)uni; (void)ctrl; return 0; }
int  studio_ai_focused(void) { return g_focused; }
void studio_ai_blur(void) { g_focused = 0; }

void studio_ai_attach_file(const char *n, const char *t, int l)
{
    if (!att_file) return;
    s_cpy(att_name, n && n[0] ? n : "file", sizeof att_name);
    if (l > 60 * 1024) l = 60 * 1024;
    memcpy(att_file, t, (unsigned long)l); att_len = l; att_pending = 1;
    g_focused = 1;
    conv_addz(ROLE_SYS, "Attached the current file to your next message.");
}
void studio_ai_attach_errors(const char *t) { (void)t; }
void studio_ai_settings(void)
{
    g_focused = 1;
    conv_addz(ROLE_SYS, "Settings: /provider openai|anthropic|gemini, "
                        "/model <id>, /key <value>, then /save.");
}

/* ---- drawing -------------------------------------------------------------- */
/* word-wrap one message body; returns the y past it */
static int draw_wrapped(int x, int y, int w, int lh, const char *s, int n,
                        fb_px c, int clip_top, int clip_bot)
{
    int i = 0;
    char line[200];
    while (i < n) {
        int ll = 0, lastspace = -1;
        while (i < n && s[i] != '\n') {
            if (ll >= 198) break;
            line[ll] = s[i]; line[ll + 1] = 0;
            if (s[i] == ' ') lastspace = ll;
            if (fb_text_w(line) > w && ll > 0) {
                if (lastspace > 0) ll = lastspace;   /* break at the last space */
                break;
            }
            ll++; i++;
        }
        line[ll] = 0;
        if (y > clip_top - lh && y < clip_bot) fb_text(x, y, line, c, -1);
        y += lh;
        /* advance past the break point */
        if (i < n && s[i] == '\n') i++;
        else if (lastspace > 0 && ll == lastspace) { i = i; while (i < n && s[i] == ' ') i++; }
    }
    return y;
}

static void draw_header(unoui_rect r, const unoui_palette *p, int hdrh)
{
    char hd[48]; int k = 0; const char *nm = kProv[cfg_provider].name;
    fb_fill_rect(r.x + 1, r.y, r.w - 1, hdrh, p->title_bg_in);
    s_cpy(hd, "AI: ", sizeof hd);
    while (nm[k] && k < 40) { hd[4+k] = nm[k]; k++; } hd[4+k] = 0;
    fb_text(r.x + 8, r.y + 3, hd, p->title_fg_in, -1);
}

void studio_ai_draw(unoui_rect r, const void *theme)
{
    const struct unoui_theme *t = (const struct unoui_theme *)theme;
    const unoui_palette *p = &t->pal;
    int lh = fb_text_h() + 2, hdrh = lh + 4, inh = lh + 6;
    int convy = r.y + hdrh, convh = r.h - hdrh - inh, i, y;

    fb_vline(r.x, r.y, r.h, p->dark);
    fb_fill_rect(r.x + 1, r.y, r.w - 1, r.h, p->win_bg);

    /* measure to auto-pin scroll to the bottom */
    { int meas = 0;
      for (i = 0; i < nmsg; i++) meas += ((int)(msg[i].len / 24) + 2) * lh + 4;
      if (ai_scroll >= (1 << 19)) { int over = meas - convh; ai_scroll = over > 0 ? over : 0; } }

    y = convy - ai_scroll;
    for (i = 0; i < nmsg; i++) {
        fb_px c = msg[i].role == ROLE_USER ? p->text
                : msg[i].role == ROLE_AI   ? p->accent : p->text_dim;
        const char *pre = msg[i].role == ROLE_USER ? "You:"
                        : msg[i].role == ROLE_AI   ? "AI:" : "*";
        if (y > convy - lh && y < convy + convh) fb_text(r.x + 6, y, pre, c, -1);
        y = draw_wrapped(r.x + 6, y + lh, r.w - 14, lh,
                         conv + msg[i].off, msg[i].len, c, convy, convy + convh);
        y += 4;
    }

    draw_header(r, p, hdrh);                          /* redraw over scrolled text */

    /* input line */
    y = r.y + r.h - inh;
    fb_hline(r.x, y, r.w, p->dark);
    fb_fill_rect(r.x + 1, y + 1, r.w - 2, inh - 1, p->field_bg);
    if (busy) fb_text(r.x + 6, y + 3, "thinking...", p->accent, -1);
    else {
        char shown[64]; int k = 0, start = in_len > 30 ? in_len - 30 : 0;
        while (in_line[start + k] && k < 60) { shown[k] = in_line[start + k]; k++; }
        if (g_focused) { shown[k] = '_'; shown[k+1] = 0; } else shown[k] = 0;
        fb_text(r.x + 6, y + 3, (k || g_focused) ? shown : "Ask the assistant...",
                (k || g_focused) ? p->field_text : p->text_dim, -1);
    }
}

int studio_ai_click(int x, int y, unoui_rect r) { (void)x; (void)y; (void)r; g_focused = 1; return 1; }
