/* ===========================================================================
 * ud_xlsx.c - the .xlsx reader (SpreadsheetML, [MS-XLSX] / ECMA-376).
 *
 * It builds the SAME `ud_xls` workbook ud_xls.c builds from BIFF8, through the
 * builder seam in ud_xls_int.h.  Everything above - ud_xls_cell_at(), the
 * merges, the number formats, UnoCalc's whole load loop - is shared and does
 * not know which parser ran.  That is the design: one cell model, two
 * spellings of the same spreadsheet.
 *
 * THE PARTS, and what each one is for:
 *
 *   xl/workbook.xml            the sheet list: name, visibility, and an r:id
 *   xl/_rels/workbook.xml.rels the r:id -> part name map
 *   xl/sharedStrings.xml       the string pool; a cell of type "s" indexes it
 *   xl/styles.xml              cellXfs -> numFmtId, and any custom numFmt code
 *   xl/worksheets/sheetN.xml   the cells
 *
 * THE ONE THING THAT SURPRISES PEOPLE about SpreadsheetML: a cell's `t`
 * attribute names the type of its `<v>`, and the default when `t` is absent is
 * NUMBER - so `<c r="A1"><v>3</v></c>` is 3 and `<c r="A1" t="s"><v>3</v></c>`
 * is shared string number three.  Reading the second as a number is the
 * classic xlsx bug, and it looks like a spreadsheet full of small integers.
 *
 * THE RELATIONSHIP INDIRECTION IS FOLLOWED, not guessed.  Sheet order in
 * workbook.xml is the order the tabs appear; which FILE each one is stored in
 * comes from the .rels part, and it is NOT "sheet1.xml is the first sheet" -
 * Excel renumbers parts as sheets are deleted, so a file whose second tab
 * lives in sheet4.xml is entirely ordinary.  Guessing gets the right answer on
 * files that were never edited and silently swaps two tabs on files that were.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_xls_int.h"
#include <string.h>

#define MAXSHEET   256
#define MAXSST     200000        /* shared strings; Excel's own cap is 2^31  */
#define TXTBUF     4096          /* one cell's text / one shared string      */
#define MAXXF      8192

/* unodoc carries no libc beyond mem-/str-, so it carries its own bounded copy. */
static void scopy(char *d, const char *s, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ---- a small owned-string pool for the shared string table ------------------
 * The strings themselves live in the workbook (ud_xls_b_str), which frees them
 * at close; this is only the index. */
typedef struct {
    const char **s;
    int n, cap;
} sst_pool;

static int sst_add(sst_pool *p, const char *s)
{
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        const char **n = (const char **)ud_alloc((unsigned long)nc * sizeof(char *));
        if (!n) return 0;
        if (p->n) memcpy(n, p->s, (unsigned long)p->n * sizeof(char *));
        ud_free(p->s);
        p->s = n; p->cap = nc;
    }
    p->s[p->n++] = s;
    return 1;
}

/* ---- "B7" -> row 6, col 1 ----------------------------------------------------
 * The reference is 1-based with letters for the column; unodoc is 0-based with
 * numbers, the same as the BIFF side. */
static int ref_to_rc(const char *s, long n, int *row, int *col)
{
    long i = 0;
    int c = 0, r = 0;
    if (!s || n <= 0) return 0;
    while (i < n && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) {
        int d = (s[i] >= 'a') ? s[i] - 'a' : s[i] - 'A';
        c = c * 26 + d + 1;
        if (c > UD_XLS_MAXCOL) return 0;
        i++;
    }
    if (!c || i >= n) return 0;
    while (i < n) {
        if (s[i] < '0' || s[i] > '9') return 0;
        r = r * 10 + (s[i] - '0');
        if (r > UD_XLS_MAXROW) return 0;
        i++;
    }
    if (!r) return 0;
    *row = r - 1;
    *col = c - 1;
    return 1;
}

/* A decimal number as SpreadsheetML writes one: always '.' for the point and
 * always ASCII, whatever the machine's locale, because the format says so. */
static double parse_num(const char *s, long n, int *ok)
{
    double v = 0, f = 0, scale = 1;
    long i = 0;
    int neg = 0, any = 0;
    *ok = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; any = 1; }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            f = f * 10 + (s[i] - '0'); scale *= 10; i++; any = 1;
        }
        v += f / scale;
    }
    if (!any) return 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        int e = 0, eneg = 0;
        i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) { eneg = s[i] == '-'; i++; }
        while (i < n && s[i] >= '0' && s[i] <= '9') { e = e * 10 + (s[i] - '0'); i++; }
        while (e-- > 0) { if (eneg) v /= 10; else v *= 10; }
    }
    *ok = 1;
    return neg ? -v : v;
}

/* Excel's seven error strings, back to the on-disk encoding the BIFF reader
 * and every consumer already speak. */
static int err_code(const char *s, long n)
{
    static const struct { const char *t; int c; } E[] = {
        { "#NULL!", UD_XE_NULL }, { "#DIV/0!", UD_XE_DIV0 },
        { "#VALUE!", UD_XE_VALUE }, { "#REF!", UD_XE_REF },
        { "#NAME?", UD_XE_NAME }, { "#NUM!", UD_XE_NUM }, { "#N/A", UD_XE_NA }
    };
    unsigned k;
    for (k = 0; k < sizeof E / sizeof E[0]; k++) {
        long l = (long)strlen(E[k].t);
        if (n == l && !strncmp(s, E[k].t, (unsigned long)l)) return E[k].c;
    }
    return UD_XE_NA;
}

/* ---- the relationship map ----------------------------------------------------
 * xl/_rels/workbook.xml.rels: Id -> Target.  A target may be absolute
 * ("/xl/worksheets/sheet1.xml") or relative to xl/ ("worksheets/sheet1.xml"),
 * and both spellings are in the wild. */
typedef struct { char id[24]; char target[112]; } xrel;

static int load_rels(ud_zip *z, const char *part, xrel *out, int max)
{
    int idx = ud_zip_find(z, part), n = 0;
    unsigned char *buf;
    long len = 0;
    ud_xml x;
    if (idx < 0) return 0;
    buf = ud_zip_load(z, idx, &len);
    if (!buf) return 0;
    ud_xml_init(&x, (const char *)buf, len);
    while (ud_xml_next(&x) && n < max) {
        char t[112];
        if (x.kind != UD_XML_START || !ud_xml_is(&x, "Relationship")) continue;
        if (!ud_xml_attr_str(&x, "Id", out[n].id, (int)sizeof out[0].id)) continue;
        if (!ud_xml_attr_str(&x, "Target", t, (int)sizeof t)) continue;
        /* normalise to a full part name */
        if (t[0] == '/') {
            scopy(out[n].target, t + 1, (int)sizeof out[0].target);
        } else {
            const char *rel = t;
            int at = 0, k = 0;
            if (rel[0] == '.' && rel[1] == '/') rel += 2;   /* "./sheet1.xml" */
            scopy(out[n].target, "xl/", (int)sizeof out[0].target);
            at = (int)strlen(out[n].target);
            while (rel[k] && at < (int)sizeof out[0].target - 1)
                out[n].target[at++] = rel[k++];
            out[n].target[at] = 0;
        }
        n++;
    }
    ud_free(buf);
    return n;
}

static const char *rel_target(const xrel *r, int n, const char *id)
{
    int i;
    if (!id || !*id) return 0;
    for (i = 0; i < n; i++) if (!strcmp(r[i].id, id)) return r[i].target;
    return 0;
}

/* ---- styles: which number format each cell format selects -------------------- */
static void load_styles(ud_zip *z, ud_xls *x)
{
    int idx = ud_zip_find(z, "xl/styles.xml"), nxf = 0, in_cellxfs = 0;
    unsigned char *buf;
    long len = 0;
    ud_xml p;
    if (idx < 0) return;
    buf = ud_zip_load(z, idx, &len);
    if (!buf) return;
    ud_xml_init(&p, (const char *)buf, len);
    while (ud_xml_next(&p)) {
        if (p.kind == UD_XML_START && ud_xml_is(&p, "numFmt")) {
            char code[128];
            long id = ud_xml_attr_int(&p, "numFmtId", -1);
            if (id >= 0 && ud_xml_attr_str(&p, "formatCode", code, (int)sizeof code))
                ud_xls_b_fmt(x, (int)id, code);
            continue;
        }
        /* cellXfs holds the formats CELLS point at; cellStyleXfs holds the
         * named styles they inherit from, and counting both would shift every
         * cell's xf index by however many named styles the file has. */
        if (p.kind == UD_XML_START && ud_xml_is(&p, "cellXfs")) { in_cellxfs = 1; continue; }
        if (p.kind == UD_XML_END && ud_xml_is(&p, "cellXfs")) { in_cellxfs = 0; continue; }
        if (in_cellxfs && p.kind == UD_XML_START && ud_xml_is(&p, "xf")) {
            if (nxf < MAXXF) ud_xls_b_xf(x, nxf, (int)ud_xml_attr_int(&p, "numFmtId", 0));
            nxf++;
        }
    }
    ud_free(buf);
}

/* ---- the shared string table --------------------------------------------------
 * Each <si> is one string, but it may be split across several <t> runs with
 * formatting between them (`<si><r><t>Hello </t></r><r><t>world</t></r></si>`),
 * and it is ONE string: the runs are its rich-text formatting, not separate
 * entries.  Concatenating them is what makes the indexes line up.
 *
 * <rPh> (phonetic guides on Japanese text) sits inside <si> and is NOT part of
 * the string; it is skipped, or every such cell reads doubled. */
static void load_sst(ud_zip *z, ud_xls *x, sst_pool *pool)
{
    int idx = ud_zip_find(z, "xl/sharedStrings.xml");
    unsigned char *buf;
    long len = 0;
    ud_xml p;
    char *txt;
    if (idx < 0) return;
    buf = ud_zip_load(z, idx, &len);
    if (!buf) return;
    txt = (char *)ud_alloc(TXTBUF);
    if (!txt) { ud_free(buf); return; }
    ud_xml_init(&p, (const char *)buf, len);
    while (ud_xml_next(&p)) {
        int depth;
        long o = 0;
        if (p.kind != UD_XML_START || !ud_xml_is(&p, "si")) continue;
        if (pool->n >= MAXSST) break;
        txt[0] = 0;
        if (p.empty) { sst_add(pool, ud_xls_b_str(x, "")); continue; }
        depth = p.depth;
        while (ud_xml_next(&p)) {
            if (p.kind == UD_XML_END && p.depth < depth) break;
            if (p.kind == UD_XML_START && ud_xml_is(&p, "rPh")) { ud_xml_skip(&p); continue; }
            if (p.kind == UD_XML_START && ud_xml_is(&p, "t"))
                o += ud_xml_inner_text(&p, txt + o, (long)TXTBUF - o);
        }
        sst_add(pool, ud_xls_b_str(x, txt));
    }
    ud_free(txt);
    ud_free(buf);
}

/* ---- one worksheet ------------------------------------------------------------ */
static void load_sheet(ud_zip *z, ud_xls *x, int sh, const char *part,
                       const sst_pool *pool)
{
    int idx = ud_zip_find(z, part);
    unsigned char *buf;
    long len = 0;
    ud_xml p;
    char *txt;
    if (idx < 0) return;
    buf = ud_zip_load(z, idx, &len);
    if (!buf) return;
    txt = (char *)ud_alloc(TXTBUF);
    if (!txt) { ud_free(buf); return; }

    ud_xml_init(&p, (const char *)buf, len);
    while (ud_xml_next(&p)) {
        if (p.kind != UD_XML_START) continue;

        if (ud_xml_is(&p, "mergeCell")) {
            char ref[48];
            if (ud_xml_attr_str(&p, "ref", ref, (int)sizeof ref)) {
                char *colon = strchr(ref, ':');
                int r0, c0, r1, c1;
                if (colon) {
                    *colon = 0;
                    if (ref_to_rc(ref, (long)strlen(ref), &r0, &c0) &&
                        ref_to_rc(colon + 1, (long)strlen(colon + 1), &r1, &c1))
                        ud_xls_b_merge(x, sh, r0, c0, r1, c1);
                }
            }
            continue;
        }

        if (ud_xml_is(&p, "c")) {
            char type[16], ref[32];
            long xf;
            int row = -1, col = -1, depth = p.depth, empty = p.empty;
            int have_v = 0, is_str_inline = 0;
            double num = 0;
            const char *sval = 0;
            char *ftext = 0;
            int kind, errc = 0, is_formula = 0, sst_i = -1;

            type[0] = 0;
            ud_xml_attr_str(&p, "t", type, (int)sizeof type);
            xf = ud_xml_attr_int(&p, "s", 0);
            if (!ud_xml_attr_str(&p, "r", ref, (int)sizeof ref) ||
                !ref_to_rc(ref, (long)strlen(ref), &row, &col)) {
                /* A cell may omit `r` and be positioned by its order in the
                 * row.  Rare, and getting it wrong silently shifts a row, so
                 * it is skipped rather than guessed at. */
                if (!empty) ud_xml_skip(&p);
                continue;
            }
            txt[0] = 0;

            if (!empty) {
                while (ud_xml_next(&p)) {
                    if (p.kind == UD_XML_END && p.depth < depth) break;
                    if (p.kind != UD_XML_START) continue;
                    if (ud_xml_is(&p, "v")) {
                        ud_xml_inner_text(&p, txt, TXTBUF);
                        have_v = 1;
                    } else if (ud_xml_is(&p, "f")) {
                        char f[512];
                        long n = ud_xml_inner_text(&p, f, (long)sizeof f);
                        is_formula = 1;
                        /* A shared formula's members carry `t="shared"` with no
                         * text of their own; only the master has the
                         * expression.  Reporting the master's text on every
                         * member would be wrong (the references are relative),
                         * so a member reports its cached value and no text -
                         * exactly what the BIFF reader does when it cannot
                         * rebase one. */
                        if (n > 0) {
                            char *withEq = (char *)ud_alloc((unsigned long)n + 2);
                            if (withEq) {
                                withEq[0] = '=';
                                memcpy(withEq + 1, f, (unsigned long)n + 1);
                                ftext = (char *)ud_xls_b_str(x, withEq);
                                ud_free(withEq);
                            }
                        }
                    } else if (ud_xml_is(&p, "is")) {
                        /* an inline string: the text is here, not in the pool */
                        ud_xml_inner_text(&p, txt, TXTBUF);
                        is_str_inline = 1;
                        have_v = 1;
                    } else {
                        ud_xml_skip(&p);
                    }
                }
            }

            /* `t` names the type of <v>, and ABSENT MEANS NUMBER.  Reading a
             * shared-string index as a number is the classic xlsx bug and it
             * looks like a sheet full of small integers. */
            if (is_str_inline || !strcmp(type, "inlineStr")) {
                kind = UD_XV_STR;
                sval = ud_xls_b_str(x, txt);
            } else if (!strcmp(type, "s")) {
                int ok = 0;
                sst_i = (int)parse_num(txt, (long)strlen(txt), &ok);
                kind = UD_XV_STR;
                sval = (ok && sst_i >= 0 && sst_i < pool->n) ? pool->s[sst_i] : "";
            } else if (!strcmp(type, "str")) {
                kind = UD_XV_STR;                       /* a formula's string */
                sval = ud_xls_b_str(x, txt);
            } else if (!strcmp(type, "b")) {
                kind = UD_XV_BOOL;
                num = (txt[0] == '1') ? 1 : 0;
            } else if (!strcmp(type, "e")) {
                kind = UD_XV_ERR;
                errc = err_code(txt, (long)strlen(txt));
            } else if (have_v && txt[0]) {
                int ok = 0;
                num = parse_num(txt, (long)strlen(txt), &ok);
                kind = ok ? UD_XV_NUM : UD_XV_STR;
                if (!ok) sval = ud_xls_b_str(x, txt);
            } else {
                /* No value at all.  A cell that carries only a style is a
                 * BLANK - it exists, and its format matters to whoever draws
                 * the grid, so it is recorded rather than dropped. */
                kind = UD_XV_EMPTY;
            }

            {
                ud_xcell *cell = ud_xls_b_cell(x, sh, row, col);
                if (!cell) continue;
                cell->kind    = kind;
                cell->num     = num;
                cell->str     = sval;
                cell->err     = errc;
                cell->xf      = (int)xf;
                cell->formula = is_formula;
                cell->ftext   = ftext;
            }
            continue;
        }

        /* Everything else - <cols>, <pageSetup>, <drawing>, conditional
         * formatting - is stepped over whole, which is what keeps a part this
         * reader does not model from derailing the walk. */
        if (ud_xml_is(&p, "sheetData") || ud_xml_is(&p, "row") ||
            ud_xml_is(&p, "worksheet") || ud_xml_is(&p, "mergeCells"))
            continue;                                   /* descend into these */
        ud_xml_skip(&p);
    }
    ud_free(txt);
    ud_free(buf);
}

/* ---- the workbook -------------------------------------------------------------- */
ud_xls *ud_xlsx_open(ud_zip *z)
{
    ud_xls *x;
    sst_pool pool;
    xrel *rels = 0;
    unsigned char *wb = 0;
    long wblen = 0;
    int nrel = 0, wbi, nsheet = 0;
    ud_xml p;
    struct { char part[112]; } part[MAXSHEET];

    if (!z) { ud_set_error("xlsx: no container"); return 0; }
    wbi = ud_zip_find(z, "xl/workbook.xml");
    if (wbi < 0) {
        ud_set_error(ud_zip_find(z, "xl/workbook.bin") >= 0
                     ? "xlsb (binary workbook) - not decoded in this build"
                     : "not a spreadsheet (no xl/workbook.xml)");
        return 0;
    }
    x = ud_xls_blank();
    if (!x) return 0;
    memset(&pool, 0, sizeof pool);

    rels = (xrel *)ud_alloc((unsigned long)MAXSHEET * sizeof(xrel));
    if (rels) {
        memset(rels, 0, (unsigned long)MAXSHEET * sizeof(xrel));
        nrel = load_rels(z, "xl/_rels/workbook.xml.rels", rels, MAXSHEET);
    }

    load_styles(z, x);
    load_sst(z, x, &pool);

    wb = ud_zip_load(z, wbi, &wblen);
    if (!wb) { ud_free(rels); ud_free(pool.s); ud_xls_close(x); return 0; }
    ud_xml_init(&p, (const char *)wb, wblen);
    while (ud_xml_next(&p)) {
        if (p.kind != UD_XML_START) continue;
        if (ud_xml_is(&p, "workbookPr")) {
            /* The 1904 epoch: rare, and getting it wrong moves every date in
             * the file by four years and a day. */
            ud_xls_b_date1904(x, ud_xml_attr_bool(&p, "date1904", 0));
            continue;
        }
        if (ud_xml_is(&p, "sheet") && nsheet < MAXSHEET) {
            char name[64], rid[24], state[16];
            const char *tgt;
            int sh;
            name[0] = rid[0] = state[0] = 0;
            ud_xml_attr_str(&p, "name", name, (int)sizeof name);
            /* r:id specifically.  A `<sheet>` happens to spell its own number
             * `sheetId`, so a local-name match works here by luck; the
             * presentation's `<p:sldId>` does not, and one rule is better
             * than two. */
            ud_xml_attr_ns_str(&p, "r", "id", rid, (int)sizeof rid);
            ud_xml_attr_str(&p, "state", state, (int)sizeof state);
            sh = ud_xls_b_sheet(x, name, strcmp(state, "hidden") &&
                                         strcmp(state, "veryHidden"));
            if (sh < 0) continue;
            tgt = rel_target(rels, nrel, rid);
            /* No relationship for it?  Then the sheet exists (its tab is real)
             * but its cells cannot be found, and an empty tab is a better
             * report than a missing one. */
            part[nsheet].part[0] = 0;
            if (tgt) {
                int k = 0;
                while (tgt[k] && k < (int)sizeof part[0].part - 1) {
                    part[nsheet].part[k] = tgt[k];
                    k++;
                }
                part[nsheet].part[k] = 0;
            }
            nsheet++;
        }
    }
    ud_free(wb);

    {
        int i;
        for (i = 0; i < nsheet; i++)
            if (part[i].part[0]) load_sheet(z, x, i, part[i].part, &pool);
    }

    ud_free(rels);
    ud_free(pool.s);
    ud_xls_built(x);
    return x;
}
