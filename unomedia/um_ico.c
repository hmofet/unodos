/* ===========================================================================
 * unomedia - the ICO/CUR decoder, written from scratch against Microsoft's
 * icon resource documentation.
 *
 * ICO is a container, not a pixel format: a 6-byte directory header, then
 * 16-byte entries, each pointing at a payload that is either a bare DIB
 * (height doubled for the XOR + AND planes) or - since Vista - a whole PNG
 * stream. So this file owns only the directory: open() picks the best
 * entry (largest pixel area, then highest bit depth; a size byte of 0
 * means 256), sniffs the payload's first bytes, and delegates to
 * um_bmp_open_dib(ico=1) or um_png_open_at() (unomedia_int.h). frame(),
 * rewind() and close() forward to whichever decoder claimed the payload.
 *
 * CUR is the same directory with type 2; its entry's planes/bitcount
 * fields hold the hotspot instead of a depth, so the CUR tiebreak is the
 * payload byte size rather than a hotspot masquerading as a depth. The
 * hotspot itself is ignored - a viewer wants the picture.
 *
 * Every directory field is distrusted: the chosen entry must lie whole
 * inside the file before a single payload byte is believed. info.format
 * reports "ICO"; w/h/alpha come from the payload.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>
#include <stdint.h>

static const um_idecoder *g_pay;    /* payload vtable to forward to        */

static uint32_t rd32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

/* ---- probe ----------------------------------------------------------------
 * The magic is weak (00 00 01 00), so beyond reserved/type/count the first
 * directory entry's reserved byte is checked too - unless the extension
 * already says ICO/CUR, in which case the header alone decides. */
static int ico_probe(const unsigned char *head, long n, const char *ext)
{
    int type, count;
    if (n < 6 || rd16(head) != 0) return 0;
    type  = rd16(head + 2);
    count = rd16(head + 4);
    if ((type != 1 && type != 2) || count < 1) return 0;
    if (!strcmp(ext, "ICO") || !strcmp(ext, "CUR")) return 1;
    return n >= 22 && (head[9] == 0 || head[9] == 255);
}

/* ---- open: pick the best entry, sniff, delegate --------------------------- */
static int ico_open(um_image_info *info)
{
    static const unsigned char png_sig[8] =
        { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    unsigned char h[6], e[16], sig[8];
    uint32_t size = 0, off = 0, fsz = (uint32_t)um_size();
    long best_area = -1, best_tie = -1;
    int  i, count, cur;

    g_pay = 0;
    if (um_read(0, h, 6) != 6)
        { um_set_error("truncated ICO header"); return 0; }
    cur   = rd16(h + 2) == 2;
    count = rd16(h + 4);

    for (i = 0; i < count; i++) {
        long area, tie;
        int  w, ht;
        if (um_read(6 + 16L * i, e, 16) != 16)
            { um_set_error("truncated ICO directory"); return 0; }
        w  = e[0] ? e[0] : 256;                   /* 0 encodes 256         */
        ht = e[1] ? e[1] : 256;
        area = (long)w * ht;
        tie  = cur ? (long)(rd32(e + 8) & 0x7FFFFFFFu)   /* payload bytes  */
                   : (long)rd16(e + 6);                  /* bit depth      */
        if (area > best_area || (area == best_area && tie > best_tie)) {
            best_area = area;
            best_tie  = tie;
            size = rd32(e + 8);
            off  = rd32(e + 12);
        }
    }
    if (size == 0 || off == 0 || off > fsz || size > fsz - off)
        { um_set_error("ICO entry points outside the file"); return 0; }

    if (um_read((long)off, sig, 8) == 8 && !memcmp(sig, png_sig, 8)) {
        if (!um_png_open_at((long)off, info)) {
            if (!um_error()[0])
                um_set_error("ICO PNG payload not decoded");
            return 0;
        }
        g_pay = &um_idec_png;
    } else {
        if (!um_bmp_open_dib((long)off, (long)(off + size), 1, info)) {
            if (!um_error()[0])
                um_set_error("ICO DIB payload not decoded");
            return 0;
        }
        g_pay = &um_idec_bmp;
    }
    info->format = "ICO";                         /* w/h/alpha: payload's  */
    return 1;
}

/* ---- forwards ------------------------------------------------------------- */
static int ico_frame(um_px *dst, int *delay_ms)
{
    if (!g_pay) { um_set_error("ICO decoder not open"); return -1; }
    return g_pay->frame(dst, delay_ms);
}

static void ico_rewind(void)
{
    if (g_pay && g_pay->rewind) g_pay->rewind();
}

static void ico_close(void)
{
    if (g_pay) {
        g_pay->close();
        g_pay = 0;
    }
}

const um_idecoder um_idec_ico =
    { "ico", ico_probe, ico_open, ico_frame, ico_rewind, ico_close };
