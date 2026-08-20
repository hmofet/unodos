/* ===========================================================================
 * ud_xml.c - the XML pull parser the OOXML readers walk documents with.
 *
 * WHY A PULL PARSER, and why this one is not general-purpose XML.  An OOXML
 * part is machine-written XML in a fixed shape: no DTDs, no entity
 * definitions, no processing instructions that mean anything, no mixed
 * content worth preserving.  What a reader needs is "give me the next start
 * tag, its attributes, and the text between tags", and it needs that over a
 * 20 MB sheet without building a tree - because a tree of a 20 MB sheet is
 * 100 MB of nodes on a machine that has a 4 MB module arena.
 *
 * So: ONE PASS, NO ALLOCATION.  The parser holds a cursor into the caller's
 * buffer and reports each event by pointing INTO it.  The one place bytes are
 * copied is ud_xml_text(), which has to un-escape entities and therefore
 * needs somewhere to put the result - and it writes into a caller-supplied
 * buffer rather than allocating.
 *
 * NAMESPACES ARE MATCHED BY LOCAL NAME.  `<w:p>`, `<p>` and
 * `<x:p xmlns:x="...">` all answer to "p".  A conforming parser would resolve
 * prefixes against the declarations in scope; OOXML uses one well-known
 * prefix per namespace and the element vocabularies do not collide, so
 * comparing local names is exact for these documents and costs nothing.  The
 * prefix is still exposed (ud_xml_prefix) for the one place it matters -
 * telling `r:id` from `id` on a relationship reference.
 *
 * WHAT IS SKIPPED SILENTLY, because it never carries document content:
 * `<?...?>`, `<!--...-->`, `<!DOCTYPE...>` and `<![CDATA[...]]>` (whose
 * contents ARE reported as text).
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include <string.h>

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int name_char(int c)
{
    return !is_space(c) && c != '>' && c != '/' && c != '=' && c != '<' && c;
}

void ud_xml_init(ud_xml *x, const char *src, long len)
{
    memset(x, 0, sizeof *x);
    x->p = src;
    x->n = len < 0 ? (long)strlen(src) : len;
    x->at = 0;
    x->kind = UD_XML_NONE;
}

/* The local name of the current element: the part after any prefix colon. */
static void split_name(const char *s, long n, const char **local, long *llen,
                       const char **pfx, long *plen)
{
    long i;
    *pfx = s; *plen = 0;
    *local = s; *llen = n;
    for (i = 0; i < n; i++)
        if (s[i] == ':') {
            *plen = i;
            *local = s + i + 1;
            *llen = n - i - 1;
            return;
        }
}

/* ---- entity decoding --------------------------------------------------------
 * The five predefined entities plus numeric character references.  A reference
 * this parser does not know is passed through verbatim rather than dropped:
 * OOXML defines no others, so an unknown one is damage, and showing "&foo;" is
 * a better report of damage than showing nothing. */
static long put_uc(unsigned long uc, char *out, long room)
{
    /* unodoc's internal text is CP-1252 (UNODOC.md); anything outside it
     * folds to '?' exactly as the binary readers do. */
    unsigned char b = ud_uc_to_cp1252((uint16_t)(uc > 0xFFFF ? 0xFFFD : uc));
    if (room < 1) return 0;
    *out = (char)b;
    return 1;
}

static long decode_entity(const char *s, long n, char *out, long room, long *used)
{
    long i = 1;                                    /* s[0] is '&' */
    *used = 0;
    if (n < 3) return 0;
    if (s[1] == '#') {
        unsigned long uc = 0;
        int hex = (n > 2 && (s[2] == 'x' || s[2] == 'X'));
        i = hex ? 3 : 2;
        for (; i < n && s[i] != ';'; i++) {
            int c = s[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            uc = uc * (unsigned long)(hex ? 16 : 10) + (unsigned long)d;
            if (uc > 0x10FFFF) return 0;
        }
        if (i >= n || s[i] != ';') return 0;
        *used = i + 1;
        return put_uc(uc, out, room);
    }
    {
        static const struct { const char *name; char ch; } ENT[] = {
            { "amp;",  '&' }, { "lt;",   '<' }, { "gt;",   '>' },
            { "quot;", '"' }, { "apos;", '\'' }
        };
        unsigned k;
        for (k = 0; k < sizeof ENT / sizeof ENT[0]; k++) {
            long l = (long)strlen(ENT[k].name);
            if (n - 1 >= l && !strncmp(s + 1, ENT[k].name, (unsigned long)l)) {
                if (room < 1) return 0;
                *out = ENT[k].ch;
                *used = 1 + l;
                return 1;
            }
        }
    }
    return 0;
}

/* ---- the pull ---------------------------------------------------------------- */
int ud_xml_next(ud_xml *x)
{
    x->kind = UD_XML_NONE;
    x->nattr = 0;
    x->tlen = 0;
    x->empty = 0;

    for (;;) {
        long s;
        if (x->at >= x->n) return 0;

        if (x->p[x->at] != '<') {
            /* character data up to the next '<' */
            s = x->at;
            while (x->at < x->n && x->p[x->at] != '<') x->at++;
            x->text = x->p + s;
            x->tlen = x->at - s;
            x->kind = UD_XML_TEXT;
            return 1;
        }

        /* '<' - which of the five things is it? */
        if (x->at + 3 < x->n && !strncmp(x->p + x->at, "<!--", 4)) {
            x->at += 4;
            while (x->at + 2 < x->n && strncmp(x->p + x->at, "-->", 3)) x->at++;
            x->at = x->at + 3 < x->n ? x->at + 3 : x->n;
            continue;
        }
        if (x->at + 8 < x->n && !strncmp(x->p + x->at, "<![CDATA[", 9)) {
            x->at += 9;
            s = x->at;
            while (x->at + 2 < x->n && strncmp(x->p + x->at, "]]>", 3)) x->at++;
            x->text = x->p + s;
            x->tlen = x->at - s;
            x->cdata = 1;
            x->at = x->at + 3 <= x->n ? x->at + 3 : x->n;
            x->kind = UD_XML_TEXT;
            return 1;
        }
        if (x->at + 1 < x->n && (x->p[x->at + 1] == '?' || x->p[x->at + 1] == '!')) {
            while (x->at < x->n && x->p[x->at] != '>') x->at++;
            if (x->at < x->n) x->at++;
            continue;
        }
        if (x->at + 1 < x->n && x->p[x->at + 1] == '/') {
            const char *nm;
            long nl;
            x->at += 2;
            s = x->at;
            while (x->at < x->n && name_char(x->p[x->at])) x->at++;
            nm = x->p + s;
            nl = x->at - s;
            split_name(nm, nl, &x->name, &x->nlen, &x->pfx, &x->plen);
            while (x->at < x->n && x->p[x->at] != '>') x->at++;
            if (x->at < x->n) x->at++;
            if (x->depth > 0) x->depth--;
            x->kind = UD_XML_END;
            return 1;
        }

        /* a start tag */
        x->at++;
        s = x->at;
        while (x->at < x->n && name_char(x->p[x->at])) x->at++;
        split_name(x->p + s, x->at - s, &x->name, &x->nlen, &x->pfx, &x->plen);

        while (x->at < x->n) {
            long as, vs;
            char q;
            while (x->at < x->n && is_space(x->p[x->at])) x->at++;
            if (x->at >= x->n) break;
            if (x->p[x->at] == '/') {
                x->empty = 1;
                x->at++;
                continue;
            }
            if (x->p[x->at] == '>') { x->at++; break; }
            as = x->at;
            while (x->at < x->n && name_char(x->p[x->at])) x->at++;
            if (x->at == as) { x->at++; continue; }        /* junk: step over */
            if (x->nattr < UD_XML_ATTRS) {
                split_name(x->p + as, x->at - as,
                           &x->attr[x->nattr].name, &x->attr[x->nattr].nlen,
                           &x->attr[x->nattr].pfx,  &x->attr[x->nattr].plen);
            }
            while (x->at < x->n && is_space(x->p[x->at])) x->at++;
            if (x->at < x->n && x->p[x->at] == '=') x->at++;
            while (x->at < x->n && is_space(x->p[x->at])) x->at++;
            q = (x->at < x->n && (x->p[x->at] == '"' || x->p[x->at] == '\''))
              ? x->p[x->at] : 0;
            if (q) x->at++;
            vs = x->at;
            if (q) while (x->at < x->n && x->p[x->at] != q) x->at++;
            else   while (x->at < x->n && !is_space(x->p[x->at]) &&
                          x->p[x->at] != '>' && x->p[x->at] != '/') x->at++;
            if (x->nattr < UD_XML_ATTRS) {
                x->attr[x->nattr].val = x->p + vs;
                x->attr[x->nattr].vlen = x->at - vs;
                x->nattr++;
            }
            if (q && x->at < x->n) x->at++;
        }
        if (!x->empty) x->depth++;
        x->kind = UD_XML_START;
        return 1;
    }
}

/* ---- what the reader asks --------------------------------------------------- */
int ud_xml_is(const ud_xml *x, const char *local)
{
    long n = (long)strlen(local);
    return x->nlen == n && !strncmp(x->name, local, (unsigned long)n);
}

const char *ud_xml_attr(const ud_xml *x, const char *name, long *len)
{
    long n = (long)strlen(name);
    int i;
    if (len) *len = 0;
    for (i = 0; i < x->nattr; i++)
        if (x->attr[i].nlen == n &&
            !strncmp(x->attr[i].name, name, (unsigned long)n)) {
            if (len) *len = x->attr[i].vlen;
            return x->attr[i].val;
        }
    return 0;
}

/* THE ONE PLACE THE PREFIX MATTERS, and the reason it is exposed at all.
 *
 * `<p:sldId id="256" r:id="rId2"/>` carries TWO attributes whose local name is
 * "id": the slide's own number and the relationship that says which part holds
 * it.  Matching on the local name alone returns whichever comes first, which
 * is the number - so every slide resolves to no part and a deck reads as three
 * empty slides.  That is precisely what happened, and it is why the
 * relationship reference is fetched by PREFIX AND NAME.
 *
 * `pfx` may be NULL or "" to mean "the one with no prefix". */
const char *ud_xml_attr_ns(const ud_xml *x, const char *pfx, const char *name,
                           long *len)
{
    long n = (long)strlen(name);
    long pn = pfx ? (long)strlen(pfx) : 0;
    int i;
    if (len) *len = 0;
    for (i = 0; i < x->nattr; i++) {
        if (x->attr[i].nlen != n ||
            strncmp(x->attr[i].name, name, (unsigned long)n)) continue;
        if (x->attr[i].plen != pn) continue;
        if (pn && strncmp(x->attr[i].pfx, pfx, (unsigned long)pn)) continue;
        if (len) *len = x->attr[i].vlen;
        return x->attr[i].val;
    }
    return 0;
}

int ud_xml_attr_ns_str(const ud_xml *x, const char *pfx, const char *name,
                       char *out, int cap)
{
    long vlen = 0;
    const char *v = ud_xml_attr_ns(x, pfx, name, &vlen);
    if (cap > 0) out[0] = 0;
    if (!v || cap <= 1) return 0;
    return (int)ud_xml_unescape(v, vlen, out, cap);
}

int ud_xml_attr_str(const ud_xml *x, const char *name, char *out, int cap)
{
    long vlen = 0;
    const char *v = ud_xml_attr(x, name, &vlen);
    if (cap > 0) out[0] = 0;
    if (!v || cap <= 1) return 0;
    return (int)ud_xml_unescape(v, vlen, out, cap);
}

long ud_xml_attr_int(const ud_xml *x, const char *name, long dflt)
{
    long vlen = 0, v = 0, i = 0;
    int neg = 0;
    const char *s = ud_xml_attr(x, name, &vlen);
    if (!s || !vlen) return dflt;
    if (s[0] == '-') { neg = 1; i = 1; }
    else if (s[0] == '+') i = 1;
    if (i >= vlen) return dflt;
    for (; i < vlen; i++) {
        if (s[i] < '0' || s[i] > '9') return dflt;
        v = v * 10 + (s[i] - '0');
        if (v > 0x7FFFFFFFL) return dflt;
    }
    return neg ? -v : v;
}

/* An OOXML boolean attribute: absent means TRUE on a toggle property
 * (`<w:b/>` is bold), and "0"/"false"/"off" mean false. */
int ud_xml_attr_bool(const ud_xml *x, const char *name, int dflt)
{
    long vlen = 0;
    const char *s = ud_xml_attr(x, name, &vlen);
    if (!s) return dflt;
    if (vlen == 1) return !(s[0] == '0');
    if (vlen == 5 && !strncmp(s, "false", 5)) return 0;
    if (vlen == 3 && !strncmp(s, "off", 3)) return 0;
    if (vlen == 4 && !strncmp(s, "true", 4)) return 1;
    if (vlen == 2 && !strncmp(s, "on", 2)) return 1;
    return dflt;
}

/* AN OOXML PART IS UTF-8, and unodoc's internal text is CP-1252 (see
 * unodoc_int.h) - so the bytes have to be DECODED, not copied.  Copying them
 * makes every accented character two characters: `café` reads as `cafÃ©`, and
 * the shared-fixture test caught exactly that by putting the .xls and the
 * .xlsx of one spreadsheet side by side.  A byte that is not valid UTF-8 is
 * passed through unchanged, because a file that is really CP-1252 in an XML
 * wrapper is better read approximately than turned into replacement marks. */
static long utf8_take(const char *s, long n, unsigned long *uc)
{
    unsigned char c = (unsigned char)s[0];
    int need, k;
    unsigned long v;
    if (c < 0x80) { *uc = c; return 1; }
    if ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07u; }
    else return 0;                                   /* a stray continuation */
    if (n < need + 1) return 0;
    for (k = 1; k <= need; k++) {
        unsigned char t = (unsigned char)s[k];
        if ((t & 0xC0) != 0x80) return 0;
        v = (v << 6) | (t & 0x3Fu);
    }
    /* reject overlong forms: they are how a validator gets bypassed, and they
     * are never what a writer produced */
    if ((need == 1 && v < 0x80) || (need == 2 && v < 0x800) ||
        (need == 3 && v < 0x10000)) return 0;
    *uc = v;
    return need + 1;
}

long ud_xml_unescape(const char *s, long n, char *out, long cap)
{
    long i = 0, o = 0;
    if (cap <= 0) return 0;
    while (i < n && o < cap - 1) {
        unsigned long uc = 0;
        long w;
        if (s[i] == '&') {
            long used = 0;
            long got = decode_entity(s + i, n - i, out + o, cap - 1 - o, &used);
            if (used) { o += got; i += used; continue; }
        }
        w = utf8_take(s + i, n - i, &uc);
        if (w <= 0) { out[o++] = s[i++]; continue; }
        o += put_uc(uc, out + o, cap - 1 - o);
        i += w;
    }
    out[o] = 0;
    return o;
}

/* Collect the text of the element the parser is currently INSIDE, following
 * nested elements, until its matching end tag.  This is how a Word paragraph
 * or a shared string is read: the text is scattered across `<w:t>` runs and
 * what the caller wants is the sentence. */
long ud_xml_inner_text(ud_xml *x, char *out, long cap)
{
    int depth = x->depth;
    long o = 0;
    if (cap > 0) out[0] = 0;
    if (x->empty) return 0;
    while (ud_xml_next(x)) {
        if (x->kind == UD_XML_TEXT) {
            o += ud_xml_unescape(x->text, x->tlen, out + o, cap - o);
        } else if (x->kind == UD_XML_END && x->depth < depth) {
            break;
        }
    }
    return o;
}

/* Skip everything inside the current element, leaving the cursor just past
 * its end tag.  The one call a reader needs for a subtree it does not
 * understand, and the reason an unknown element cannot derail the walk. */
void ud_xml_skip(ud_xml *x)
{
    int depth = x->depth;
    if (x->empty) return;
    while (ud_xml_next(x))
        if (x->kind == UD_XML_END && x->depth < depth) return;
}
