/* ===========================================================================
 * unomedia - the GIF decoder (87a + 89a), written from scratch against the
 * GIF89a specification.
 *
 * Structure: on open() the block stream is walked once CHEAPLY - sub-blocks
 * skipped by their length bytes, no LZW work - so info.frames is exact
 * before a single pixel is decoded; then the walk cursor rewinds to the
 * first block and frame() replays it for real. Each frame() composites into
 * the caller's PERSISTENT canvas: the canvas is cleared to transparent
 * black before frame 0, a frame draws its rect honouring the GCE
 * transparency index, and its disposal method is applied just before the
 * NEXT frame draws (0/1 = leave, 2 = clear the rect to transparent - the
 * -coalesce convention every renderer follows, 3 = restore-previous via an
 * um_alloc'd backup of the covered rect). After the last frame, frame()
 * returns 0; rewind() restarts at frame 0 - looping is the caller's call,
 * so the NETSCAPE loop-count extension is walked over and ignored.
 *
 * LZW is decoded streaming through the data sub-blocks: variable code
 * width 3..12 bits LSB-first, clear / EOI codes, and the "deferred clear"
 * case (a full 4096-entry table keeps translating until a clear arrives).
 * The table is prefix links, so expansion chains strictly descend and
 * cannot loop; output is clamped to the frame rect no matter what the
 * stream claims. Interlaced frames route rows through the 4-pass order.
 *
 * info.alpha = 1 if any frame carries a transparency index, frame 0 does
 * not cover the canvas, or any frame disposes to background - the three
 * ways a transparent canvas pixel can survive to the screen.
 *
 * All decode state (~21 KB: two colour tables, the LZW arrays, one
 * sub-block buffer) lives in a single um_alloc'd block, freed in close().
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>
#include <stdint.h>

typedef struct {
    int  cw, ch;                /* canvas (logical screen)                  */
    int  gct_n;
    um_px gct[256], lct[256];
    long first_off;             /* first block after LSD + GCT              */
    long off;                   /* walk cursor                              */
    int  nframes, idx;
    int  started;               /* canvas cleared for this playthrough      */
    /* pending Graphic Control Extension (applies to the next image)       */
    int  gce, trans_on, trans_idx, delay_ms, disp;
    /* the previous frame's disposal, applied before the next draw         */
    int  pdisp, px, py, pw, ph;
    um_px *backup;              /* disposal-3 backup of the covered rect    */
    /* LZW table + expansion stack                                          */
    uint16_t prefix[4096];
    unsigned char suffix[4096];
    unsigned char stack[4096];
    /* sub-block byte stream                                                */
    long soff;
    int  sleft, spos;
    unsigned char sbuf[256];
    uint32_t bitbuf;
    int  bitcnt;
    /* frame paint cursor                                                   */
    int  p_fx, p_fy, p_fw, p_fh, p_col, p_row, p_pass, p_ilace, p_trans;
    long p_left;
    const um_px *p_tab;
} gif_st;

static gif_st *st;

static uint16_t rd16(const unsigned char *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static int rdb(long off, unsigned char *d, int n)     /* exact read       */
{ return um_read(off, d, n) == n; }

/* skip a sub-block chain (length byte + data, 0 terminates) */
static int skip_subs(long *off)
{
    unsigned char l;
    for (;;) {
        if (!rdb(*off, &l, 1)) return 0;
        (*off)++;
        if (!l) return 1;
        *off += l;
    }
}

/* ---- the census: count frames + spot alpha, no pixel work ----------------- */
static int census(void)
{
    long o = st->first_off;
    unsigned char b, g[6], d[9];
    int trans = 0, bgdisp = 0, covered = 0, alpha;

    st->nframes = 0;
    for (;;) {
        if (!rdb(o, &b, 1)) {
            if (st->nframes) break;      /* trailer lost - frames are whole */
            um_set_error("truncated GIF");
            return 0;
        }
        o++;
        if (b == 0x3B) break;                     /* trailer               */
        if (b == 0x00) continue;                  /* stray zero, tolerated */
        if (b == 0x21) {                          /* extension             */
            if (!rdb(o, g, 1)) { um_set_error("truncated GIF"); return 0; }
            o++;
            if (g[0] == 0xF9 && rdb(o, g, 5) && g[0] == 4) {
                if (g[1] & 1) trans = 1;
                if (((g[1] >> 2) & 7) == 2) bgdisp = 1;
            }
            if (!skip_subs(&o)) { um_set_error("truncated GIF"); return 0; }
        } else if (b == 0x2C) {                   /* image descriptor      */
            if (!rdb(o, d, 9)) { um_set_error("truncated GIF"); return 0; }
            if (st->nframes == 0)
                covered = rd16(d) == 0 && rd16(d + 2) == 0 &&
                          rd16(d + 4) == (uint16_t)st->cw &&
                          rd16(d + 6) == (uint16_t)st->ch;
            o += 9;
            if (d[8] & 0x80) o += 3L << ((d[8] & 7) + 1);
            o++;                                  /* LZW min-code byte     */
            if (!skip_subs(&o)) { um_set_error("truncated GIF"); return 0; }
            st->nframes++;
        } else {
            um_set_error("corrupt GIF block stream");
            return 0;
        }
    }
    if (!st->nframes) { um_set_error("GIF contains no image"); return 0; }
    alpha = trans || bgdisp || !covered;
    return 1 + alpha;                             /* 1 = opaque, 2 = alpha */
}

/* ---- LZW ------------------------------------------------------------------ */
static int sb_byte(void)             /* -1 = terminator reached, -2 = trunc */
{
    unsigned char l;
    while (st->spos >= st->sleft) {
        if (!rdb(st->soff, &l, 1)) return -2;
        st->soff++;
        if (!l) return -1;
        if (!rdb(st->soff, st->sbuf, l)) return -2;
        st->soff += l;
        st->sleft = l;
        st->spos = 0;
    }
    return st->sbuf[st->spos++];
}

static int code_read(int width)      /* code, or -1 end / -2 truncated     */
{
    int b, c;
    while (st->bitcnt < width) {
        if ((b = sb_byte()) < 0) return b;
        st->bitbuf |= (uint32_t)b << st->bitcnt;
        st->bitcnt += 8;
    }
    c = (int)(st->bitbuf & ((1u << width) - 1));
    st->bitbuf >>= width;
    st->bitcnt  -= width;
    return c;
}

/* one output pixel, routed through interlace + transparency + canvas clip */
static void emit(um_px *dst, int v)
{
    static const int istart[4] = { 0, 4, 2, 1 };
    static const int istep[4]  = { 8, 8, 4, 2 };

    if (st->p_left <= 0) return;                  /* rect full - drop      */
    if (v != st->p_trans) {
        int x = st->p_fx + st->p_col, y = st->p_fy + st->p_row;
        if (x < st->cw && y < st->ch)
            dst[(long)y * st->cw + x] = st->p_tab[v & 255];
    }
    st->p_left--;
    if (++st->p_col == st->p_fw) {
        st->p_col = 0;
        if (!st->p_ilace) {
            st->p_row++;
        } else {
            st->p_row += istep[st->p_pass];
            while (st->p_row >= st->p_fh && st->p_pass < 3)
                st->p_row = istart[++st->p_pass];
        }
    }
}

static int lzw_decode(um_px *dst, int min)
{
    int clear = 1 << min, eoi = clear + 1;
    int next = eoi + 1, width = min + 1;
    int prev = -1, code, c, k, sp;

    st->sleft = st->spos = 0;
    st->bitbuf = 0;
    st->bitcnt = 0;

    for (;;) {
        code = code_read(width);
        if (code == -2) { um_set_error("truncated GIF image data"); return 0; }
        if (code == -1) {                         /* data ran out          */
            if (st->p_left > 0)
                { um_set_error("GIF image data ended early"); return 0; }
            return 1;                             /* EOI omitted - fine    */
        }
        if (code == clear) {
            next = eoi + 1; width = min + 1; prev = -1;
            continue;
        }
        if (code == eoi) break;

        if (prev < 0) {                           /* first code = a root   */
            if (code >= clear)
                { um_set_error("bad GIF LZW stream"); return 0; }
            emit(dst, code);
            prev = code;
            continue;
        }
        if (code > next)
            { um_set_error("bad GIF LZW stream"); return 0; }

        /* expand string(code) - or string(prev)+firstchar for KwKwK      */
        sp = 0;
        c = (code == next) ? prev : code;
        while (c >= clear) {                      /* prefix links descend  */
            st->stack[sp++] = st->suffix[c];
            c = st->prefix[c];
        }
        k = c;                                    /* the string's root     */
        emit(dst, k);
        while (sp > 0) emit(dst, st->stack[--sp]);
        if (code == next) emit(dst, k);           /* the string + its root */
        if (next < 4096) {
            st->prefix[next] = (uint16_t)prev;
            st->suffix[next] = (unsigned char)k;
            next++;
            if (next == (1 << width) && width < 12) width++;
        }
        prev = code;
    }
    /* drain the remaining sub-blocks to the terminator */
    while ((c = sb_byte()) >= 0) { }
    if (c == -2) { um_set_error("truncated GIF image data"); return 0; }
    if (st->p_left > 0)
        { um_set_error("GIF image data ended early"); return 0; }
    return 1;
}

/* ---- colour tables -------------------------------------------------------- */
static int load_ct(long *off, int n, um_px *tab)
{
    unsigned char t[768];
    int i;
    if (!rdb(*off, t, 3 * n)) return 0;
    *off += 3L * n;
    for (i = 0; i < n; i++)
        tab[i] = UM_PX(t[3 * i], t[3 * i + 1], t[3 * i + 2], 255);
    for (; i < 256; i++)
        tab[i] = UM_PX(0, 0, 0, 255);
    return 1;
}

/* ---- one frame ------------------------------------------------------------ */
static void rect_clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > st->cw) *w = st->cw - *x;
    if (*y + *h > st->ch) *h = st->ch - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

static int decode_frame(um_px *dst, int *delay_ms)
{
    unsigned char d[9], mc;
    int fx, fy, fw, fh, disp, bx, by, bw, bh, r;
    const um_px *tab = st->gct_n ? st->gct : 0;

    if (!rdb(st->off, d, 9))
        { um_set_error("truncated GIF image descriptor"); return -1; }
    st->off += 9;
    fx = rd16(d); fy = rd16(d + 2); fw = rd16(d + 4); fh = rd16(d + 6);
    if (fw <= 0 || fh <= 0)
        { um_set_error("empty GIF frame"); return -1; }

    /* the previous frame's disposal happens before this one draws */
    if (st->idx > 0) {
        if (st->pdisp == 2) {
            for (r = 0; r < st->ph; r++)
                memset(dst + (long)(st->py + r) * st->cw + st->px, 0,
                       (unsigned long)st->pw * 4);
        } else if (st->pdisp == 3 && st->backup) {
            for (r = 0; r < st->ph; r++)
                memcpy(dst + (long)(st->py + r) * st->cw + st->px,
                       st->backup + (long)r * st->pw,
                       (unsigned long)st->pw * 4);
        }
    }
    if (st->backup) { um_free(st->backup); st->backup = 0; }

    if (d[8] & 0x80) {                            /* local colour table    */
        int n = 2 << (d[8] & 7);
        if (!load_ct(&st->off, n, st->lct))
            { um_set_error("truncated GIF colour table"); return -1; }
        tab = st->lct;
    }
    if (!tab)
        { um_set_error("GIF frame has no colour table"); return -1; }

    disp = st->gce ? st->disp : 0;
    *delay_ms = st->gce ? st->delay_ms : 0;

    /* covered rect, clipped to the canvas, for disposal bookkeeping */
    bx = fx; by = fy; bw = fw; bh = fh;
    rect_clip(&bx, &by, &bw, &bh);
    if (disp == 3 && bw > 0 && bh > 0) {          /* backup for restore    */
        st->backup = (um_px *)um_alloc((unsigned long)bw * bh * 4);
        if (!st->backup) { um_set_error("out of memory"); return -1; }
        for (r = 0; r < bh; r++)
            memcpy(st->backup + (long)r * bw,
                   dst + (long)(by + r) * st->cw + bx,
                   (unsigned long)bw * 4);
    }
    st->pdisp = disp;
    st->px = bx; st->py = by; st->pw = bw; st->ph = bh;

    if (!rdb(st->off, &mc, 1))
        { um_set_error("truncated GIF image data"); return -1; }
    st->off++;
    if (mc < 1 || mc > 11)
        { um_set_error("bad GIF LZW code size"); return -1; }

    st->p_fx = fx; st->p_fy = fy; st->p_fw = fw; st->p_fh = fh;
    st->p_col = 0; st->p_row = 0; st->p_pass = 0;
    st->p_ilace = (d[8] & 0x40) != 0;
    st->p_trans = (st->gce && st->trans_on) ? st->trans_idx : -1;
    st->p_left = (long)fw * fh;
    st->p_tab = tab;
    st->soff = st->off;

    if (!lzw_decode(dst, mc)) return -1;
    st->off = st->soff;                           /* past the terminator   */
    st->gce = 0;
    st->idx++;
    return 1;
}

/* ---- vtable --------------------------------------------------------------- */
static int gif_probe(const unsigned char *head, long n, const char *ext)
{
    (void)ext;
    return n >= 6 && (!memcmp(head, "GIF87a", 6) || !memcmp(head, "GIF89a", 6));
}

static void gif_close(void)
{
    if (st) {
        um_free(st->backup);
        um_free(st);
        st = 0;
    }
}

static int gif_open(um_image_info *info)
{
    unsigned char h[13];
    long o = 13, cw, ch;
    int a;

    gif_close();
    if (!rdb(0, h, 13)) { um_set_error("truncated GIF header"); return 0; }
    cw = rd16(h + 6);
    ch = rd16(h + 8);
    if (cw <= 0 || ch <= 0 || cw > UM_MAX_DIM || ch > UM_MAX_DIM ||
        cw * ch > UM_MAX_PIXELS)
        { um_set_error("GIF dimensions out of range"); return 0; }

    st = (gif_st *)um_alloc(sizeof *st);
    if (!st) { um_set_error("out of memory"); return 0; }
    memset(st, 0, sizeof *st);
    st->cw = (int)cw;
    st->ch = (int)ch;
    if (h[10] & 0x80) {                           /* global colour table   */
        st->gct_n = 2 << (h[10] & 7);
        if (!load_ct(&o, st->gct_n, st->gct))
            { um_set_error("truncated GIF colour table"); gif_close(); return 0; }
    }
    st->first_off = st->off = o;

    a = census();
    if (!a) { gif_close(); return 0; }

    info->format = "GIF";
    info->w = st->cw;
    info->h = st->ch;
    info->bpp = (h[10] & 7) + 1;                  /* GCT depth             */
    info->alpha = a - 1;
    info->frames = st->nframes;
    return 1;
}

static int gif_frame(um_px *dst, int *delay_ms)
{
    unsigned char b, g[5];

    if (!st) { um_set_error("GIF decoder not open"); return -1; }
    if (st->idx == 0 && !st->started) {
        memset(dst, 0, (unsigned long)st->cw * st->ch * 4);
        st->started = 1;
    }
    for (;;) {
        if (!rdb(st->off, &b, 1)) return 0;       /* trailer lost at EOF   */
        st->off++;
        if (b == 0x3B) return 0;                  /* trailer: animation end */
        if (b == 0x00) continue;
        if (b == 0x2C) return decode_frame(dst, delay_ms);
        if (b != 0x21)
            { um_set_error("corrupt GIF block stream"); return -1; }
        if (!rdb(st->off, &b, 1))
            { um_set_error("truncated GIF"); return -1; }
        st->off++;
        if (b == 0xF9 && rdb(st->off, g, 5) && g[0] == 4) {
            st->disp      = (g[1] >> 2) & 7;      /* Graphic Control Ext.  */
            st->trans_on  = g[1] & 1;
            st->delay_ms  = (int)rd16(g + 2) * 10;/* raw value, 0 included */
            st->trans_idx = g[4];
            st->gce = 1;
        }
        if (!skip_subs(&st->off))                 /* NETSCAPE et al: skip  */
            { um_set_error("truncated GIF"); return -1; }
    }
}

static void gif_rewind(void)
{
    if (!st) return;
    st->off = st->first_off;
    st->idx = 0;
    st->started = 0;
    st->gce = 0;
    st->pdisp = 0;
    um_free(st->backup);
    st->backup = 0;
}

const um_idecoder um_idec_gif =
    { "gif", gif_probe, gif_open, gif_frame, gif_rewind, gif_close };
