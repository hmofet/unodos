/* ===========================================================================
 * ud_escher.c - OfficeArt / Escher [MS-ODRAW], read side.
 *
 * The drawing layer shared by all three formats: shapes, their geometry, and
 * the picture store.  A .ppt slide's visuals are Escher, a .doc's floating
 * objects are Escher, and an .xls chart sits on Escher - so this is written
 * standalone, taking a byte range rather than a document, and each format
 * hands it whatever range holds its drawing.
 *
 * Escher uses the SAME 8-byte record header as the PowerPoint stream around
 * it (ver/instance, type, length), which is why one walker serves both.  Two
 * things about it are not obvious and both matter here:
 *
 *   - A SHAPE'S TYPE IS IN ITS HEADER, not its body.  The msofbtSp record's
 *     recInstance field - the top 12 bits of the first u16 - is the shape
 *     type (rectangle, ellipse, the ~200 autoshapes).  The body is only the
 *     shape id and some flags.
 *   - PROPERTIES ARE A SORTED ARRAY WITH THE BIG ONES OUT OF LINE.  An FOPT
 *     record's recInstance says how many properties follow; each is a u16 id
 *     plus a u32 value, and when the id's top bit says "complex" that value
 *     is a LENGTH, with the data appended after the whole array.  Read it as
 *     a value and every property after it is still fine, but the blob is
 *     lost; read the array as fixed-size records and you desynchronise.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include <string.h>

#define E_HDR         8
#define E_SPCONTAINER 0xF004
#define E_SP          0xF00A
#define E_OPT         0xF00B
#define E_CLIENTANCH  0xF010
#define E_CHILDANCH   0xF00F

/* the FOPT ids this build understands; the rest are carried but unnamed */
#define P_GEOTEXT     0x0080   /* the shape's text id                       */
#define P_FILLCOLOR   0x0181
#define P_FILLED      0x01BF
#define P_LINECOLOR   0x01C0
#define P_LINEWIDTH   0x01CB
#define P_LINED       0x01FF

static int ehdr(const unsigned char *b, long n, long at,
                int *inst, int *type, long *len)
{
    if (at < 0 || at + E_HDR > n) return 0;
    *inst = (int)(ud_rd16(b + at) >> 4);
    *type = (int)ud_rd16(b + at + 2);
    *len  = (long)ud_rd32(b + at + 4);
    if (*len < 0 || at + E_HDR + *len > n) return 0;
    return 1;
}

/* Escher's container test is the low nibble, exactly as in the PPT stream. */
static int eis_container(const unsigned char *b, long at)
{ return (ud_rd16(b + at) & 0x000F) == 0x000F; }

/* Find one property in an FOPT record.  Returns 1 and sets *val when it is
 * present.  The array is walked rather than indexed, because a complex
 * property's value is a length and the data sits after the array - the whole
 * point of the format's layout. */
static int fopt_get(const unsigned char *b, long at, long len, int nprop,
                    int want, uint32_t *val)
{
    long i;
    for (i = 0; i < nprop; i++) {
        long p = at + i * 6;
        uint16_t id;
        if (p + 6 > at + len) return 0;
        id = ud_rd16(b + p);
        if ((int)(id & 0x3FFF) == want) {
            *val = ud_rd32(b + p + 2);
            return 1;
        }
    }
    return 0;
}

/* Walk one SpContainer and fill in a shape. */
static void shape_from(const unsigned char *b, long n, long at, long end,
                       ud_shape *s)
{
    memset(s, 0, sizeof *s);
    s->fill_color = s->line_color = -1;
    while (at + E_HDR <= end) {
        int inst, type;
        long len, body;
        if (!ehdr(b, n, at, &inst, &type, &len)) return;
        body = at + E_HDR;
        if (type == E_SP) {
            s->kind = inst;                    /* the type lives in the hdr */
            if (len >= 8) {
                s->id    = (long)ud_rd32(b + body);
                s->group = (ud_rd32(b + body + 4) & 0x0002) != 0;
            }
        } else if (type == E_OPT) {
            uint32_t v;
            if (fopt_get(b, body, len, inst, P_FILLCOLOR, &v)) s->fill_color = (long)v;
            if (fopt_get(b, body, len, inst, P_LINECOLOR, &v)) s->line_color = (long)v;
            if (fopt_get(b, body, len, inst, P_LINEWIDTH, &v)) s->line_width = (long)v;
            if (fopt_get(b, body, len, inst, P_FILLED,    &v)) s->filled = (v & 0x10000) ? (int)(v & 1) : 1;
            if (fopt_get(b, body, len, inst, P_LINED,     &v)) s->lined  = (v & 0x80000) ? (int)((v >> 3) & 1) : 1;
            if (fopt_get(b, body, len, inst, P_GEOTEXT,   &v)) s->text_id = (long)v;
        } else if (type == E_CLIENTANCH || type == E_CHILDANCH) {
            /* The anchor record is HOST-DEFINED, which is the whole reason
               Escher is format-neutral - so its shape is decided by length,
               not by assuming one host.  PowerPoint writes four int16 in
               top/left/right/bottom order; a child anchor (and Excel's
               client anchor) is four int32 left/top/right/bottom. */
            if (len >= 16) {
                s->x0 = (long)(int32_t)ud_rd32(b + body);
                s->y0 = (long)(int32_t)ud_rd32(b + body + 4);
                s->x1 = (long)(int32_t)ud_rd32(b + body + 8);
                s->y1 = (long)(int32_t)ud_rd32(b + body + 12);
            } else if (len >= 8) {
                s->y0 = (long)(int16_t)ud_rd16(b + body);
                s->x0 = (long)(int16_t)ud_rd16(b + body + 2);
                s->x1 = (long)(int16_t)ud_rd16(b + body + 4);
                s->y1 = (long)(int16_t)ud_rd16(b + body + 6);
            }
        }
        at = body + len;
    }
}

/* Collect every SpContainer in a byte range, descending through group
 * containers.  Returns how many were written (bounded by `max`). */
int ud_escher_shapes(const unsigned char *b, long n, long at, long end,
                     ud_shape *out, int max, int depth)
{
    int got = 0;
    if (depth > 16 || !b || !out || max <= 0) return 0;
    if (end > n) end = n;
    while (at + E_HDR <= end && got < max) {
        int inst, type;
        long len, body;
        if (!ehdr(b, n, at, &inst, &type, &len)) break;
        body = at + E_HDR;
        if (type == E_SPCONTAINER) {
            shape_from(b, n, body, body + len, &out[got]);
            got++;
        } else if (eis_container(b, at)) {
            got += ud_escher_shapes(b, n, body, body + len,
                                    out + got, max - got, depth + 1);
        }
        at = body + len;
    }
    return got;
}
