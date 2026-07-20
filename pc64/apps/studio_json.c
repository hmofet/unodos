/* ===========================================================================
 * Studio - JSON for the AI client.  A bounds-checked emitter and a tolerant
 * dotted-path extractor; both freestanding (no libc beyond mem/str imports).
 * The extractor is a lenient recursive-descent walker: it doesn't validate
 * the whole document, it just navigates object keys and array indices to the
 * requested value and decodes the string it finds.
 * ======================================================================== */
#include "studio_json.h"

unsigned long strlen(const char *s);

/* ---- emitter -------------------------------------------------------------- */
void jz_raw(char *buf, int *pos, int cap, const char *s)
{
    int p = *pos;
    while (*s && p < cap - 1) buf[p++] = *s++;
    buf[p] = 0;
    *pos = p;
}

void jz_strn(char *buf, int *pos, int cap, const char *s, int n)
{
    int p = *pos, i;
    if (p < cap - 1) buf[p++] = '"';
    for (i = 0; i < n && p < cap - 8; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  buf[p++] = '\\'; buf[p++] = '"';  break;
        case '\\': buf[p++] = '\\'; buf[p++] = '\\'; break;
        case '\n': buf[p++] = '\\'; buf[p++] = 'n';  break;
        case '\r': buf[p++] = '\\'; buf[p++] = 'r';  break;
        case '\t': buf[p++] = '\\'; buf[p++] = 't';  break;
        default:
            if (c < 0x20) {                          /* \u00XX */
                static const char *hex = "0123456789abcdef";
                buf[p++] = '\\'; buf[p++] = 'u'; buf[p++] = '0'; buf[p++] = '0';
                buf[p++] = hex[(c >> 4) & 0xF]; buf[p++] = hex[c & 0xF];
            } else buf[p++] = (char)c;
        }
    }
    if (p < cap - 1) buf[p++] = '"';
    buf[p] = 0;
    *pos = p;
}

void jz_str(char *buf, int *pos, int cap, const char *s)
{ jz_strn(buf, pos, cap, s, (int)strlen(s)); }

/* ---- extractor ------------------------------------------------------------ */
static const char *skip_ws(const char *p)
{ while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

/* advance past one complete JSON value (object/array/string/number/literal) */
static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {                         /* skip a string body */
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                if (!*p) break;
            } else if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p + 1; }
        }
        return p;
    }
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        return *p ? p + 1 : p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* match key name against segment [seg,seg+segn) */
static int key_eq(const char *key, int keyn, const char *seg, int segn)
{
    int i;
    if (keyn != segn) return 0;
    for (i = 0; i < keyn; i++) if (key[i] != seg[i]) return 0;
    return 1;
}

/* find the value for one path segment inside the value at p */
static const char *step(const char *p, const char *seg, int segn)
{
    p = skip_ws(p);
    if (*p == '{') {
        p++;
        for (;;) {
            const char *k;
            int kn;
            p = skip_ws(p);
            if (*p == '}' || !*p) return 0;
            if (*p != '"') return 0;
            k = ++p;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            kn = (int)(p - k);
            if (*p == '"') p++;
            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);
            if (key_eq(k, kn, seg, segn)) return p;   /* value starts here */
            p = skip_value(p);
            p = skip_ws(p);
            if (*p == ',') p++;
        }
    }
    if (*p == '[') {
        int want = 0, i = 0, j;
        for (j = 0; j < segn; j++) {
            if (seg[j] < '0' || seg[j] > '9') return 0;
            want = want * 10 + (seg[j] - '0');
        }
        p++;
        for (;;) {
            p = skip_ws(p);
            if (*p == ']' || !*p) return 0;
            if (i == want) return p;
            p = skip_value(p);
            p = skip_ws(p);
            if (*p == ',') p++;
            i++;
        }
    }
    return 0;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decode a JSON string body [p..] into out; p points just after the opening
 * quote.  Returns decoded length. */
static int decode_string(const char *p, char *out, int outmax)
{
    int o = 0;
    while (*p && *p != '"' && o < outmax - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case '/': out[o++] = '/';  break;
            case '"': out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case 'u': {
                int h0 = hexv(p[1]), h1 = hexv(p[2]), h2 = hexv(p[3]), h3 = hexv(p[4]);
                unsigned cp;
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) break;
                cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    int g0 = hexv(p[3]), g1 = hexv(p[4]), g2 = hexv(p[5]), g3 = hexv(p[6]);
                    if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
                        unsigned lo = (unsigned)((g0<<12)|(g1<<8)|(g2<<4)|g3);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                /* encode UTF-8 */
                if (cp < 0x80) { if (o < outmax-1) out[o++] = (char)cp; }
                else if (cp < 0x800) {
                    if (o < outmax-2) { out[o++] = (char)(0xC0|(cp>>6)); out[o++] = (char)(0x80|(cp&0x3F)); }
                } else if (cp < 0x10000) {
                    if (o < outmax-3) { out[o++] = (char)(0xE0|(cp>>12)); out[o++] = (char)(0x80|((cp>>6)&0x3F)); out[o++] = (char)(0x80|(cp&0x3F)); }
                } else {
                    if (o < outmax-4) { out[o++] = (char)(0xF0|(cp>>18)); out[o++] = (char)(0x80|((cp>>12)&0x3F)); out[o++] = (char)(0x80|((cp>>6)&0x3F)); out[o++] = (char)(0x80|(cp&0x3F)); }
                }
                break;
            }
            default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return o;
}

int jz_get_string(const char *json, const char *path, char *out, int outmax)
{
    const char *p = json;
    const char *seg = path;
    if (outmax > 0) out[0] = 0;
    for (;;) {
        const char *dot = seg;
        int segn;
        while (*dot && *dot != '.') dot++;
        segn = (int)(dot - seg);
        p = step(p, seg, segn);
        if (!p) return -1;
        if (!*dot) break;                            /* last segment */
        seg = dot + 1;
    }
    p = skip_ws(p);
    if (*p != '"') return -1;
    return decode_string(p + 1, out, outmax);
}
