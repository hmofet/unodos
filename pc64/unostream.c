/* ===========================================================================
 * unostream - guest-side screen streamer. See unostream.h for the wire
 * protocol and pc64/UNOSTREAM.md for the contract.
 *
 * Shape: a small state machine (idle -> connecting -> streaming) pumped once
 * per shell frame from the pc64_uui.c frame loop. On each tick, if a frame is
 * due (1000/fps ms since the last), it captures + encodes AT MOST ONE frame
 * into a staging buffer and then trickles it into the non-blocking socket,
 * keeping a partial-send offset across ticks. It never blocks the frame loop:
 * net_send queues what fits (0 when the 8 KB socket send buffer is full) and
 * we simply resume next tick. A stall (> 5 s with no byte accepted) or a
 * remote close stands the stream down.
 *
 * The QOI encoder + 32x32 tile-delta machinery is LIFTED from
 * unoauto_screen.c (same lane: both are unoautomate-adjacent capture paths;
 * the framing is deliberately byte-compatible with its strip+manifest shape)
 * rather than called through its API, for two reasons:
 *   - uno_screen_grab_delta() diffs against the LIVE-VIEW snapshot, which the
 *     `screen grab delta` remote-desktop path owns; sharing it would corrupt
 *     both diff streams. unostream needs its own snapshot, like the recorder.
 *   - every pixel here is read through a CURSOR OVERLAY: the same arrow glyph
 *     the present path composites (kCursor in uefi_main.c) is blended over
 *     fb[] at read time, so the pointer is visible in the video - screen
 *     recordings famously lack it. Reading through the overlay also makes the
 *     per-tile hashes see the cursor, so the tiles it leaves and enters are
 *     dirty automatically and deltas carry pointer motion. Nothing is ever
 *     written into the live fb[].
 *
 * Ships in production (unconditional in build.sh): the URC gate row (OBSERVE)
 * is the privilege boundary, not a compile flag - same policy as `screen`.
 * ======================================================================== */
#include "unostream.h"
#include "fb.h"          /* fb[], FB_W/FB_H (runtime), FB_MAX_W/H, fb_px */
#include "netsock.h"     /* net_socket/net_connect/net_send/..., u8/u16 */
#include "pc64_http.h"   /* pc64_net_up() */

/* freestanding libc + kernel symbols (no public header; the unoauto_remote.c
 * idiom for cross-file reads that need no contract of their own) */
void *memcpy(void *, const void *, unsigned long);
unsigned long long uno_dbg_uptime_ms(void);    /* monotonic ms (prod + debug) */
void  uno_pc64_mouse(int *x, int *y, int *btn);/* uefi_main.c: pointer coords */
int   uno_pc64_input_locked(void);             /* a security dialog is modal  */

/* ---- geometry ------------------------------------------------------------ */
#define UST_TILE 32
#ifdef FB_MAX_W
#  define UST_MAX_W FB_MAX_W
#  define UST_MAX_H FB_MAX_H
#else
#  define UST_MAX_W FB_W
#  define UST_MAX_H FB_H
#endif
#define UST_MAXCOLS  ((UST_MAX_W + UST_TILE - 1) / UST_TILE)
#define UST_MAXROWS  ((UST_MAX_H + UST_TILE - 1) / UST_TILE)
#define UST_MAXTILES (UST_MAXCOLS * UST_MAXROWS)

/* staging buffer: [hello?][8-byte header][payload]. 2 MB holds any QOI
 * keyframe of the flat-colour desktop at native res (same budget as the
 * `screen` verb's SCREEN_MAX); an overflow drops the frame, never blocks. */
#define UST_BUF   (2 * 1024 * 1024)
#define UST_HELLO 16
#define UST_HDR   8
#define UST_KEY_EVERY 120              /* force a keyframe every N frames     */
#define UST_CONNECT_MS 10000           /* dial timeout                        */
#define UST_STALL_MS   5000            /* no byte accepted for this long = dead */

/* ---- state --------------------------------------------------------------- */
enum { UST_IDLE = 0, UST_CONNECTING, UST_STREAMING };
static int      g_state = UST_IDLE;
static int      g_sock  = -1;
static u8       g_ip[4];
static u16      g_port;
static int      g_fps = 30, g_scale = 1;
static unsigned g_interval_ms = 33;
static unsigned long long g_next_ms;       /* next frame due                  */
static unsigned long long g_deadline_ms;   /* connect timeout                 */
static unsigned long long g_progress_ms;   /* last time the socket took bytes */

static unsigned           g_sent_frames;
static unsigned long long g_sent_bytes;
static unsigned           g_drops;         /* skipped frames + stalls/closes  */
static unsigned           g_since_key;     /* frames since the last keyframe  */

static unsigned char g_buf[UST_BUF];       /* staged hello/frame bytes        */
static int           g_len, g_off;         /* staged length / send offset     */

/* delta snapshot (this stream's OWN, like the recorder's - see header note) */
static unsigned       g_snap[UST_MAXTILES];
static unsigned short g_chg[UST_MAXTILES];
static int g_snap_valid;
static int g_ew, g_eh, g_cols, g_rows;     /* emitted geometry of the stream  */

/* ---- cursor overlay -------------------------------------------------------
 * The SAME arrow the present path draws (kCursor / cursor_row in uefi_main.c,
 * 9x15, 'B' black outline / 'W' white fill / ' ' transparent). Duplicated
 * here because it is file-static there; byte-for-byte the same glyph. */
static const char *UST_CURSOR[15] = {
    "B","BB","BWB","BWWB","BWWWB","BWWWWB","BWWWWWB","BWWWWWWB",
    "BWWWWBBBB","BWWBWB","BWB BWB","BB  BWB","B    BWB","      BWB","       BB"
};
static int g_cur_x, g_cur_y;               /* latched once per captured frame */

/* fb pixel at FRAMEBUFFER coords (x,y) with the cursor blended over it. */
static fb_px ov_px_fb(int x, int y)
{
    fb_px v = fb[y * FB_W + x];
    int r = y - g_cur_y, c = x - g_cur_x;
    if (r >= 0 && r < 15 && c >= 0 && c < 9) {
        const char *row = UST_CURSOR[r];
        int i;
        for (i = 0; i < c && row[i]; i++) ;
        if (i == c && row[c]) {
            if (row[c] == 'B') return FB_RGB(0, 0, 0);
            if (row[c] == 'W') return FB_RGB(0xFF, 0xFF, 0xFF);
        }
    }
    return v;
}

/* emitted pixel (ex,ey) under `scale` (nearest-neighbour, like the grabs) */
static fb_px ov_px(int ex, int ey, int scale)
{
    return ov_px_fb(ex * scale, ey * scale);
}

/* ---- QOI encoder (incremental; lifted from unoauto_screen.c) ------------- */
#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF  0x40
#define QOI_OP_LUMA  0x80
#define QOI_OP_RUN   0xc0
#define QOI_OP_RGB   0xfe
#define QOI_OP_RGBA  0xff

typedef struct { unsigned char r, g, b, a; } qpx;
static int qhash(qpx p) { return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) & 63; }

typedef struct {
    unsigned char *out;
    int   cap, o, run;
    qpx   prev;
    qpx   index[64];
} qenc;

static int q_begin(qenc *e, unsigned char *out, int cap, int ew, int eh)
{
    int i;
    if (cap < 14 + 8) return -1;
    e->out = out; e->cap = cap; e->o = 0; e->run = 0;
    e->prev.r = e->prev.g = e->prev.b = 0; e->prev.a = 255;
    for (i = 0; i < 64; i++) { e->index[i].r = e->index[i].g = e->index[i].b = e->index[i].a = 0; }
    out[e->o++] = 'q'; out[e->o++] = 'o'; out[e->o++] = 'i'; out[e->o++] = 'f';
    out[e->o++] = (unsigned char)(ew >> 24); out[e->o++] = (unsigned char)(ew >> 16);
    out[e->o++] = (unsigned char)(ew >> 8);  out[e->o++] = (unsigned char)ew;
    out[e->o++] = (unsigned char)(eh >> 24); out[e->o++] = (unsigned char)(eh >> 16);
    out[e->o++] = (unsigned char)(eh >> 8);  out[e->o++] = (unsigned char)eh;
    out[e->o++] = 4;
    out[e->o++] = 0;
    return 0;
}

static int q_px(qenc *e, qpx px)
{
    if (e->o + 8 > e->cap) return -1;
    if (px.r == e->prev.r && px.g == e->prev.g && px.b == e->prev.b && px.a == e->prev.a) {
        e->run++;
        if (e->run == 62) { e->out[e->o++] = (unsigned char)(QOI_OP_RUN | (e->run - 1)); e->run = 0; }
        return 0;
    }
    {
        int ip;
        if (e->run > 0) { e->out[e->o++] = (unsigned char)(QOI_OP_RUN | (e->run - 1)); e->run = 0; }
        ip = qhash(px);
        if (e->index[ip].r == px.r && e->index[ip].g == px.g &&
            e->index[ip].b == px.b && e->index[ip].a == px.a) {
            e->out[e->o++] = (unsigned char)(QOI_OP_INDEX | ip);
        } else {
            e->index[ip] = px;
            if (px.a == e->prev.a) {
                int vr = (int)px.r - (int)e->prev.r;
                int vg = (int)px.g - (int)e->prev.g;
                int vb = (int)px.b - (int)e->prev.b;
                int dr_dg = vr - vg, db_dg = vb - vg;
                if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) {
                    e->out[e->o++] = (unsigned char)(QOI_OP_DIFF |
                        ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
                } else if (dr_dg > -9 && dr_dg < 8 && vg > -33 && vg < 32 &&
                           db_dg > -9 && db_dg < 8) {
                    e->out[e->o++] = (unsigned char)(QOI_OP_LUMA | (vg + 32));
                    e->out[e->o++] = (unsigned char)(((dr_dg + 8) << 4) | (db_dg + 8));
                } else {
                    e->out[e->o++] = QOI_OP_RGB;
                    e->out[e->o++] = px.r; e->out[e->o++] = px.g; e->out[e->o++] = px.b;
                }
            } else {
                e->out[e->o++] = QOI_OP_RGBA;
                e->out[e->o++] = px.r; e->out[e->o++] = px.g; e->out[e->o++] = px.b; e->out[e->o++] = px.a;
            }
        }
        e->prev = px;
    }
    return 0;
}

static int q_end(qenc *e)
{
    if (e->run > 0) {
        if (e->o + 8 > e->cap) return -1;
        e->out[e->o++] = (unsigned char)(QOI_OP_RUN | (e->run - 1));
        e->run = 0;
    }
    if (e->o + 8 > e->cap) return -1;
    e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 0;
    e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 0; e->out[e->o++] = 1;
    return e->o;
}

static qpx px_of(fb_px v)
{
    qpx p;
    p.r = (unsigned char)(v & 0xff);
    p.g = (unsigned char)((v >> 8) & 0xff);
    p.b = (unsigned char)((v >> 16) & 0xff);
    p.a = (unsigned char)((v >> 24) & 0xff);
    return p;
}

/* ---- tiling (through the cursor overlay) --------------------------------- */
static unsigned tile_hash(int tc, int tr)
{
    int x0 = tc * UST_TILE, x1 = x0 + UST_TILE;
    int y0 = tr * UST_TILE, y1 = y0 + UST_TILE;
    unsigned h = 2166136261u;
    int x, y;
    if (x1 > g_ew) x1 = g_ew;
    if (y1 > g_eh) y1 = g_eh;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            h = (h ^ (unsigned)ov_px(x, y, g_scale)) * 16777619u;
    return h;
}

static void snap_fill(void)
{
    int tr, tc;
    for (tr = 0; tr < g_rows; tr++)
        for (tc = 0; tc < g_cols; tc++)
            g_snap[tr * g_cols + tc] = tile_hash(tc, tr);
    g_snap_valid = 1;
}

/* diff against the snapshot, refreshing changed hashes; returns the count */
static int snap_diff(void)
{
    int tr, tc, nch = 0;
    for (tr = 0; tr < g_rows; tr++) {
        for (tc = 0; tc < g_cols; tc++) {
            unsigned h = tile_hash(tc, tr);
            int t = tr * g_cols + tc;
            if (h != g_snap[t]) {
                g_snap[t] = h;
                g_chg[nch++] = (unsigned short)t;
            }
        }
    }
    return nch;
}

static int encode_full(unsigned char *out, int cap)
{
    qenc e;
    int x, y;
    if (q_begin(&e, out, cap, g_ew, g_eh) < 0) return -1;
    for (y = 0; y < g_eh; y++)
        for (x = 0; x < g_ew; x++)
            if (q_px(&e, px_of(ov_px(x, y, g_scale))) < 0) return -1;
    return q_end(&e);
}

static int encode_strip(unsigned char *out, int cap, int nch)
{
    qenc e;
    int i;
    if (q_begin(&e, out, cap, UST_TILE, nch * UST_TILE) < 0) return -1;
    for (i = 0; i < nch; i++) {
        int t = g_chg[i], tc = t % g_cols, tr = t / g_cols;
        int bx = tc * UST_TILE, by = tr * UST_TILE, cr;
        for (cr = 0; cr < UST_TILE; cr++) {
            int yy = by + cr, x;
            for (x = 0; x < UST_TILE; x++) {
                int sx = bx + x;
                qpx p;
                if (sx < g_ew && yy < g_eh) p = px_of(ov_px(sx, yy, g_scale));
                else { p.r = p.g = p.b = 0; p.a = 0; }
                if (q_px(&e, p) < 0) return -1;
            }
        }
    }
    return q_end(&e);
}

/* ---- wire staging -------------------------------------------------------- */
static void put_u16le(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff); }
static void put_u32le(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static int stage_hello(unsigned char *p)
{
    p[0] = 'U'; p[1] = 'N'; p[2] = 'S'; p[3] = 'M';
    p[4] = 1;                       /* ver  */
    p[5] = 0;                       /* pad  */
    p[6] = (unsigned char)g_fps;
    p[7] = (unsigned char)g_scale;
    put_u16le(p + 8,  (unsigned)g_ew);
    put_u16le(p + 10, (unsigned)g_eh);
    put_u32le(p + 12, 0);
    return UST_HELLO;
}

/* Capture + encode one frame into g_buf. Returns 1 with g_len/g_off set (the
 * tick pump takes it from here), or 0 when the frame was dropped (encode
 * overflow - counted by the caller). Handles geometry changes: a stream whose
 * FB_W/FB_H (or first frame) does not match the current hello geometry stages
 * a fresh hello in FRONT of a forced keyframe - the receiver treats a
 * mid-stream hello as a stream reset. */
static int stage_frame(void)
{
    int ew = FB_W / g_scale, eh = FB_H / g_scale;
    int hello = 0, off = 0, payload, type, nch = 0;
    unsigned char *hdr, *pl;
    int plcap;

    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    if (ew != g_ew || eh != g_eh) {            /* first frame, or res changed */
        g_ew = ew; g_eh = eh;
        g_cols = (ew + UST_TILE - 1) / UST_TILE;
        g_rows = (eh + UST_TILE - 1) / UST_TILE;
        g_snap_valid = 0;
        hello = 1;
    }

    /* latch the cursor once, so hash + encode see the same frame */
    { int b; uno_pc64_mouse(&g_cur_x, &g_cur_y, &b); (void)b; }

    if (hello) off = stage_hello(g_buf);
    hdr = g_buf + off;
    pl  = hdr + UST_HDR;
    plcap = UST_BUF - off - UST_HDR;

    if (!g_snap_valid || g_since_key >= UST_KEY_EVERY) {
keyframe:
        payload = encode_full(pl, plcap);
        if (payload < 0) {                     /* can't fit: drop this frame  */
            g_snap_valid = 0;                  /* force a keyframe retry next */
            return 0;
        }
        snap_fill();                           /* baseline for the next delta */
        type = 0;
        g_since_key = 0;
    } else {
        nch = snap_diff();
        if (nch > 0) {
            int strip = encode_strip(pl, plcap - nch * 2, nch);
            if (strip < 0) goto keyframe;      /* too much changed: keyframe  */
            {   int i, mo = strip;             /* trailing u16le manifest     */
                for (i = 0; i < nch; i++) { put_u16le(pl + mo, g_chg[i]); mo += 2; }
                payload = mo;
            }
        } else {
            payload = 0;                       /* valid "nothing changed"     */
        }
        type = 1;
        g_since_key++;
    }

    hdr[0] = (unsigned char)type;
    hdr[1] = 0;
    put_u16le(hdr + 2, 0);                     /* reserved */
    put_u32le(hdr + 4, (unsigned)payload);
    g_len = off + UST_HDR + payload;
    g_off = 0;
    return 1;
}

/* ---- state machine ------------------------------------------------------- */
static void stream_down(void)
{
    if (g_sock >= 0) { net_sock_close(g_sock); g_sock = -1; }
    g_state = UST_IDLE;
    g_len = g_off = 0;
}

void unostream_tick(void)
{
    unsigned long long now;
    int st;

    if (g_state == UST_IDLE) return;
    net_poll();
    now = uno_dbg_uptime_ms();
    st  = (g_sock >= 0) ? net_sock_state(g_sock) : TCP_CLOSED;

    if (g_state == UST_CONNECTING) {
        if (st == TCP_ESTABLISHED) {
            g_state = UST_STREAMING;
            g_ew = g_eh = 0;                   /* first stage_frame sends hello */
            g_snap_valid = 0;
            g_since_key = 0;
            g_next_ms = now;
            g_progress_ms = now;
        } else if (st == TCP_CLOSED || st == TCP_DONE || now > g_deadline_ms) {
            g_drops++;
            stream_down();
        }
        return;
    }

    /* UST_STREAMING */
    if (st != TCP_ESTABLISHED) {               /* peer closed / link lost     */
        g_drops++;
        stream_down();
        return;
    }

    /* pump the staged bytes; net_send queues what fits (0 = buffer full) */
    while (g_off < g_len) {
        int chunk = g_len - g_off, r;
        if (chunk > 4096) chunk = 4096;
        r = net_send(g_sock, g_buf + g_off, chunk);
        if (r <= 0) break;
        g_off += r;
        g_sent_bytes += (unsigned)r;
        g_progress_ms = now;
    }
    if (g_off >= g_len && g_len > 0) {         /* frame fully handed to TCP   */
        g_sent_frames++;
        g_len = g_off = 0;
    }

    if (g_len > 0) {                           /* still draining a frame      */
        if (now - g_progress_ms > UST_STALL_MS) {  /* dead peer / black hole  */
            g_drops++;
            stream_down();
            return;
        }
        if (now >= g_next_ms) {                /* a due frame we can't take   */
            g_drops++;
            g_next_ms = now + g_interval_ms;
        }
        return;
    }

    if (now >= g_next_ms) {
        /* Mirror the `screen` verb's privacy rule: while a security dialog is
         * modal at the console, capture nothing (the dialog holds exactly the
         * credentials being typed). The stream stays up; frames just pause.
         * (The frame loop doesn't tick us during a modal loop anyway - this
         * guard covers the transition frames.) */
        if (uno_pc64_input_locked()) { g_next_ms = now + g_interval_ms; return; }
        if (!stage_frame()) g_drops++;         /* encode overflow: dropped    */
        /* fixed cadence with catch-up clamp: never schedule into the past */
        g_next_ms += g_interval_ms;
        if (g_next_ms <= now) g_next_ms = now + g_interval_ms;
    }
}

/* ---- URC verb ------------------------------------------------------------ */
/* tiny local copies of the URC arg helpers (unoauto_remote.c keeps its own
 * private; four lines each is cheaper than a new shared contract) */
static char *ust_tok(char **s)
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
static long ust_atol(const char *s)
{
    long v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}
static int ust_streq(const char *a, const char *b)
{
    if (!a) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
typedef struct { char *p; int cap, len; } ustsb;
static void usb_c(ustsb *b, char c)        { if (b->len < b->cap - 1) b->p[b->len++] = c; }
static void usb_s(ustsb *b, const char *s) { while (*s) usb_c(b, *s++); }
static void usb_u(ustsb *b, unsigned long long v)
{
    char t[24]; int n = 0;
    if (!v) { usb_c(b, '0'); return; }
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n) usb_c(b, t[--n]);
}

static int status_line(char *out, int cap)
{
    ustsb b; b.p = out; b.cap = cap; b.len = 0;
    usb_s(&b, "on=");    usb_u(&b, g_state != UST_IDLE);
    usb_s(&b, " fps=");  usb_u(&b, (unsigned)g_fps);
    usb_s(&b, " sent="); usb_u(&b, g_sent_frames);
    usb_s(&b, " bytes=");usb_u(&b, g_sent_bytes);
    usb_s(&b, " drops=");usb_u(&b, g_drops);
    out[b.len] = 0;
    return b.len;
}

static int err_reply(char *out, int cap, const char *msg)
{
    int i = 0;
    for (; msg[i] && i < cap - 1; i++) out[i] = msg[i];
    out[i] = 0;
    return -1;
}

int unostream_cmd(char *line, char *out, int cap)
{
    char *sub = ust_tok(&line);
    if (!out || cap < 2) return -1;

    if (!sub || ust_streq(sub, "status"))
        return status_line(out, cap);

    if (ust_streq(sub, "stop")) {
        stream_down();
        return status_line(out, cap);
    }

    if (ust_streq(sub, "start")) {
        char *a_ip = ust_tok(&line), *a_port = ust_tok(&line);
        char *a_fps = ust_tok(&line), *a_scale = ust_tok(&line);
        u8 ip[4]; int port, fps, scale, i;
        char *p;
        if (g_state != UST_IDLE)
            return err_reply(out, cap, "already streaming (stream stop first)");
        if (!a_ip || !a_port)
            return err_reply(out, cap, "usage: stream start <ip4> <port> [fps] [scale]");
        if (uno_pc64_input_locked())
            return err_reply(out, cap, "refused (a security dialog is open at the console)");
        p = a_ip;
        for (i = 0; i < 4; i++) {
            if (*p < '0' || *p > '9') return err_reply(out, cap, "bad-ip");
            ip[i] = (u8)ust_atol(p);
            while (*p >= '0' && *p <= '9') p++;
            if (i < 3) { if (*p != '.') return err_reply(out, cap, "bad-ip"); p++; }
        }
        port = (int)ust_atol(a_port);
        if (port < 1 || port > 65535) return err_reply(out, cap, "bad-port");
        fps = a_fps ? (int)ust_atol(a_fps) : 30;
        if (fps < 1)  fps = 1;
        if (fps > 60) fps = 60;
        scale = a_scale ? (int)ust_atol(a_scale) : 1;
        if (scale < 1) scale = 1;
        if (scale > 8) scale = 8;
        if (!pc64_net_up())
            return err_reply(out, cap, "no-network");
        g_sock = net_socket(SOCK_TCP);
        if (g_sock < 0)
            return err_reply(out, cap, "no-socket (table full)");
        if (net_connect(g_sock, ip, (u16)port) < 0) {
            net_sock_close(g_sock); g_sock = -1;
            return err_reply(out, cap, "connect-failed");
        }
        for (i = 0; i < 4; i++) g_ip[i] = ip[i];
        g_port = (u16)port;
        g_fps = fps; g_scale = scale;
        g_interval_ms = 1000u / (unsigned)fps;
        if (g_interval_ms < 1) g_interval_ms = 1;
        g_sent_frames = 0; g_sent_bytes = 0; g_drops = 0;
        g_len = g_off = 0;
        g_deadline_ms = uno_dbg_uptime_ms() + UST_CONNECT_MS;
        g_state = UST_CONNECTING;
        {
            ustsb b; b.p = out; b.cap = cap; b.len = 0;
            usb_s(&b, "dialing ");
            usb_u(&b, ip[0]); usb_c(&b, '.'); usb_u(&b, ip[1]); usb_c(&b, '.');
            usb_u(&b, ip[2]); usb_c(&b, '.'); usb_u(&b, ip[3]); usb_c(&b, ':');
            usb_u(&b, (unsigned)port);
            usb_s(&b, " fps="); usb_u(&b, (unsigned)fps);
            usb_s(&b, " scale="); usb_u(&b, (unsigned)scale);
            out[b.len] = 0;
            return b.len;
        }
    }

    return err_reply(out, cap, "usage: stream start <ip4> <port> [fps] [scale] | stop | status");
}
