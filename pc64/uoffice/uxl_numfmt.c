/* ===========================================================================
 * uxl_numfmt.c - Excel's number-format language (phase 9).
 *
 * Excel's display fidelity lives here: the same double is "1234.5678",
 * "1,234.57", "123457%", "$1,234.57" or "12-May-97" depending only on the
 * format code, and getting this wrong makes every other correct number look
 * wrong.
 *
 * A code is up to four SECTIONS separated by ';' - positive; negative; zero;
 * text - and a section is a run of placeholders (# 0 ? , . %), literals, and
 * date-time pictures.  Missing sections fall back the way Excel's do: one
 * section serves everything, two make the second the negative form.
 * ======================================================================== */
#include "uocalc.h"

static double f_floor(double v)
{ double t = (double)(long long)v; return (v < 0 && t != v) ? t - 1 : t; }
static double f_pow10(int n)
{ double r = 1; while (n-- > 0) r *= 10; return r; }

static const char *kBuiltin[UXL_FMT_NBUILTIN] = {
    "General",            /* UXL_FMT_GENERAL  */
    "0",                  /* INT              */
    "0.00",               /* 2DP              */
    "#,##0",              /* THOUS            */
    "#,##0.00",           /* THOUS2           */
    "0%",                 /* PCT              */
    "0.00%",              /* PCT2             */
    "0.00E+00",           /* SCI              */
    "$#,##0.00",          /* CURRENCY         */
    "d-mmm-yy",           /* DATE             */
    "h:mm",               /* TIME             */
    "d-mmm-yy h:mm",      /* DATETIME         */
    "@"                   /* TEXT             */
};
const char *uxl_fmt_code(int id)
{ return (id >= 0 && id < UXL_FMT_NBUILTIN) ? kBuiltin[id] : "General"; }

/* ---- General ----------------------------------------------------------------
 * Excel shows at most 15 significant digits and trims trailing zeros - the
 * same convention unodoc's ud_num_text follows, and for the same reason: a
 * spreadsheet that prints 0.1 as 0.100000000000000006 is unusable. */
int uxl_general(double v, char *out, int cap)
{
    int n = 0, i, digits;
    double a;
    long long ip;
    if (!out || cap < 24) return 0;
    if (v != v) { out[0] = 0; return 0; }
    if (v < 0) { out[n++] = '-'; v = -v; }
    if (v == 0) { out[n++] = '0'; out[n] = 0; return n; }

    /* very large or very small falls back to scientific, as Excel does */
    if (v >= 1e15 || (v > 0 && v < 1e-4)) {
        int ex = 0;
        while (v >= 10) { v /= 10; ex++; }
        while (v < 1)  { v *= 10; ex--; }
        n += uxl_general(v, out + n, cap - n);
        out[n++] = 'E';
        out[n++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        if (ex < 10) out[n++] = '0';
        { char d[8]; int m = 0;
          do { d[m++] = (char)('0' + ex % 10); ex /= 10; } while (ex);
          while (m) out[n++] = d[--m]; }
        out[n] = 0;
        return n;
    }

    ip = (long long)v;
    a = v - (double)ip;
    {   char d[24];
        int m = 0;
        long long q = ip;
        do { d[m++] = (char)('0' + (int)(q % 10)); q /= 10; } while (q && m < 24);
        digits = m;
        while (m) out[n++] = d[--m];
    }
    /* as many fraction digits as fit inside fifteen significant */
    {
        int room = 15 - digits;
        if (room > 9) room = 9;
        if (room > 0 && a > 0) {
            char frac[12];
            int m = 0, k;
            double t = a;
            for (k = 0; k < room; k++) {
                t *= 10;
                frac[m++] = (char)('0' + (int)t);
                t -= f_floor(t);
            }
            /* round the last digit, then trim */
            if (t >= 0.5) {
                int j = m - 1;
                while (j >= 0) {
                    if (frac[j] != '9') { frac[j]++; break; }
                    frac[j--] = '0';
                }
                if (j < 0) {                 /* carried into the integer     */
                    int carry = 1, p = n - 1;
                    while (p >= 0 && carry) {
                        if (out[p] < '0' || out[p] > '9') break;
                        if (out[p] != '9') { out[p]++; carry = 0; }
                        else out[p] = '0';
                        p--;
                    }
                    if (carry) {
                        for (i = n; i > 0; i--) out[i] = out[i - 1];
                        out[0] = '1';
                        n++;
                    }
                }
            }
            while (m > 0 && frac[m - 1] == '0') m--;
            if (m > 0) {
                out[n++] = '.';
                for (k = 0; k < m; k++) out[n++] = frac[k];
            }
        }
    }
    out[n] = 0;
    return n;
}

/* ---- section selection ------------------------------------------------------ */
static const char *section(const char *code, int want, int *found)
{
    /* returns a pointer to the `want`th section (0..3), or NULL */
    int n = 0;
    const char *p = code, *start = code;
    *found = 0;
    while (*p) {
        if (*p == ';') {
            if (n == want) { *found = 1; return start; }
            n++;
            start = p + 1;
        }
        p++;
    }
    if (n == want) { *found = 1; return start; }
    return 0;
}
static int sec_len(const char *s)
{ int n = 0; while (s[n] && s[n] != ';') n++; return n; }

/* Is this section the word "General"?  COMPARED, not sniffed: the first cut
 * counted 'G' characters and passed anything with enough of them, so
 * "General" fell through to the numeric path and printed itself as a literal
 * prefix - "General1235". */
static int sec_is_general(const char *s, int n)
{
    static const char *g = "general";
    int i;
    if (n != 7) return 0;
    for (i = 0; i < 7; i++) {
        int c = s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != g[i]) return 0;
    }
    return 1;
}

/* Does the section contain date-time picture characters? */
static int is_datetime(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == 'y' || c == 'Y' || c == 'd' || c == 'D' ||
            c == 'h' || c == 'H' || c == 's' || c == 'S') return 1;
        if ((c == 'm' || c == 'M')) return 1;
    }
    return 0;
}

/* ---- the 1900 serial, leap-year bug included --------------------------------
 * Excel treats 1900 as a leap year because Lotus did; serial 60 is the 29th
 * of February 1900, a day that did not exist.  Reproducing that is not a bug
 * here, it is the point: every date in every real workbook is offset by it. */
static void serial_to_date(double serial, int *yy, int *mm, int *dd)
{
    long n = (long)f_floor(serial);
    long y = 1900, m = 1;
    static const int md[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (n >= 60) n--;                 /* skip the phantom 29 Feb 1900       */
    n--;                              /* serial 1 is 1 Jan 1900             */
    for (;;) {
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        long len = leap ? 366 : 365;
        if (n < len) break;
        n -= len;
        y++;
    }
    for (;;) {
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        int len = md[m - 1] + ((m == 2 && leap) ? 1 : 0);
        if (n < len) break;
        n -= len;
        m++;
    }
    *yy = (int)y; *mm = (int)m; *dd = (int)n + 1;
}

static int put_num(char *out, int n, int cap, long v, int pad)
{
    char d[16];
    int m = 0;
    if (v < 0) v = 0;
    do { d[m++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && m < 16);
    while (m < pad && m < 16) d[m++] = '0';
    while (m && n < cap - 1) out[n++] = d[--m];
    return n;
}

static const char *kMon[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec" };

static int fmt_datetime(double v, const char *s, int slen, char *out, int cap)
{
    int y, mo, d, n = 0, i = 0;
    double frac = v - f_floor(v);
    int hh = (int)(frac * 24), mi, ss;
    frac = frac * 24 - hh;
    mi = (int)(frac * 60);
    frac = frac * 60 - mi;
    ss = (int)(frac * 60 + 0.5);
    if (ss >= 60) { ss = 0; mi++; }
    if (mi >= 60) { mi = 0; hh++; }
    serial_to_date(v, &y, &mo, &d);

    while (i < slen && n < cap - 1) {
        char c = s[i];
        int run = 1;
        while (i + run < slen && s[i + run] == c) run++;
        if (c == 'y' || c == 'Y') {
            n = put_num(out, n, cap, run <= 2 ? y % 100 : y, run <= 2 ? 2 : 4);
        } else if (c == 'd' || c == 'D') {
            n = put_num(out, n, cap, d, run >= 2 ? 2 : 1);
        } else if (c == 'h' || c == 'H') {
            n = put_num(out, n, cap, hh, run >= 2 ? 2 : 1);
        } else if (c == 's' || c == 'S') {
            n = put_num(out, n, cap, ss, run >= 2 ? 2 : 1);
        } else if (c == 'm' || c == 'M') {
            /* 'm' is minutes next to an hour or a second, months otherwise -
             * Excel's one genuinely context-sensitive picture character */
            int prev = i > 0 ? s[i - 1] : 0, nxt = (i + run < slen) ? s[i + run] : 0;
            int minute = (prev == 'h' || prev == 'H' || prev == ':' ||
                          nxt == 's' || nxt == 'S');
            if (minute) n = put_num(out, n, cap, mi, run >= 2 ? 2 : 1);
            else if (run >= 3) {
                const char *nm = kMon[(mo - 1) % 12];
                int k;
                for (k = 0; nm[k] && n < cap - 1; k++) out[n++] = nm[k];
            } else n = put_num(out, n, cap, mo, run >= 2 ? 2 : 1);
        } else {
            int k;
            for (k = 0; k < run && n < cap - 1; k++) out[n++] = c;
        }
        i += run;
    }
    out[n] = 0;
    return n;
}

/* ---- the numeric path -------------------------------------------------------- */
static int fmt_number(double v, const char *s, int slen, char *out, int cap)
{
    int i, decimals = 0, thousands = 0, pct = 0, seen_dot = 0;
    int intdigits = 0, n = 0;
    long long ip;
    double a;
    char digits[32];
    int nd = 0;

    for (i = 0; i < slen; i++) {
        char c = s[i];
        if (c == '%') pct = 1;
        else if (c == '.') seen_dot = 1;
        else if (c == '0' || c == '#' || c == '?') {
            if (seen_dot) decimals++; else intdigits++;
        } else if (c == ',' && seen_dot == 0 && intdigits > 0) thousands = 1;
    }
    if (pct) v *= 100;

    if (v < 0) { out[n++] = '-'; v = -v; }
    /* round to the requested decimals FIRST, so 0.995 at two places is 1.00
     * rather than 0.99 with a rounded tail */
    {
        double m = f_pow10(decimals);
        v = f_floor(v * m + 0.5) / m;
    }
    ip = (long long)v;
    a = v - (double)ip;
    {
        long long q = ip;
        do { digits[nd++] = (char)('0' + (int)(q % 10)); q /= 10; }
        while (q && nd < 31);
    }
    {
        int k;
        for (k = nd - 1; k >= 0 && n < cap - 1; k--) {
            out[n++] = digits[k];
            if (thousands && k > 0 && (k % 3) == 0) out[n++] = ',';
        }
    }
    if (decimals > 0 && n < cap - 2) {
        int k;
        out[n++] = '.';
        for (k = 0; k < decimals && n < cap - 1; k++) {
            a *= 10;
            out[n++] = (char)('0' + (int)(a + 1e-9));
            a -= f_floor(a + 1e-9);
        }
    }
    /* literal prefixes and suffixes: currency signs, %, and quoted runs */
    {
        char pre[16], suf[16];
        int np = 0, ns = 0, started = 0;
        for (i = 0; i < slen; i++) {
            char c = s[i];
            if (c == '0' || c == '#' || c == '?' || c == '.' || c == ',') {
                started = 1;
                continue;
            }
            if (c == '"') { i++; while (i < slen && s[i] != '"') {
                    if (!started && np < 15) pre[np++] = s[i];
                    else if (started && ns < 15) suf[ns++] = s[i];
                    i++; }
                continue; }
            if (c == '[') { while (i < slen && s[i] != ']') i++; continue; }
            if (c == '\\' && i + 1 < slen) { i++;
                if (!started && np < 15) pre[np++] = s[i];
                else if (started && ns < 15) suf[ns++] = s[i];
                continue; }
            if (!started) { if (np < 15) pre[np++] = c; }
            else { if (ns < 15) suf[ns++] = c; }
        }
        pre[np] = 0; suf[ns] = 0;
        if (np) {
            int k, shift = np, sign = (out[0] == '-') ? 1 : 0;
            if (n + shift < cap - 1) {
                for (k = n; k >= sign; k--) out[k + shift] = out[k];
                for (k = 0; k < np; k++) out[sign + k] = pre[k];
                n += shift;
            }
        }
        { int k; for (k = 0; k < ns && n < cap - 1; k++) out[n++] = suf[k]; }
    }
    out[n] = 0;
    return n;
}

int uxl_format(double v, const char *code, char *out, int cap)
{
    const char *sec;
    int found, slen, want;
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!code || !*code) return uxl_general(v, out, cap);
    want = (v < 0) ? 1 : (v == 0 ? 2 : 0);
    sec = section(code, want, &found);
    if (!found) { sec = section(code, 0, &found); want = 0; }
    if (!found) return uxl_general(v, out, cap);
    slen = sec_len(sec);
    if (!slen) { out[0] = 0; return 0; }
    if (sec_is_general(sec, slen)) return uxl_general(v, out, cap);
    /* the negative section supplies its own sign, so hand it a positive */
    if (want == 1) v = -v;
    if (is_datetime(sec, slen)) return fmt_datetime(v, sec, slen, out, cap);
    return fmt_number(v, sec, slen, out, cap);
}

int uxl_format_text(const char *s, const char *code, char *out, int cap)
{
    const char *sec;
    int found, slen, i, n = 0;
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!s) return 0;
    sec = code ? section(code, 3, &found) : 0;   /* the fourth section      */
    if (!code || !found) {
        while (s[n] && n < cap - 1) { out[n] = s[n]; n++; }
        out[n] = 0;
        return n;
    }
    slen = sec_len(sec);
    for (i = 0; i < slen && n < cap - 1; i++) {
        if (sec[i] == '@') { int k = 0; while (s[k] && n < cap - 1) out[n++] = s[k++]; }
        else if (sec[i] == '"') { i++; while (i < slen && sec[i] != '"' && n < cap - 1) out[n++] = sec[i++]; }
        else out[n++] = sec[i];
    }
    out[n] = 0;
    return n;
}
