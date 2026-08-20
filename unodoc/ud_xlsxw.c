/* ===========================================================================
 * ud_xlsxw.c - writing .xlsx (SpreadsheetML).
 *
 * The OOXML half of ud_xlsw_save: it takes the SAME ud_xlsw the BIFF writer
 * takes, so an app builds a workbook once and chooses the format at the point
 * it saves.  Reading the model back goes through the accessor seam in
 * ud_ooxw_int.h; the container is ud_ooxz.c.
 *
 * WHAT IS NOT WRITTEN: fonts, colours, borders, column widths, charts and
 * images - exactly as the BIFF writer does not write them.  This serialises
 * what the model holds, which is sheets, values, formulas, number formats and
 * merges, and inventing a richer file than the model would be inventing
 * content.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ooxw_int.h"
#include "ud_ooxz.h"
#include <string.h>

/* ===========================================================================
 * .xlsx
 * ======================================================================== */

/* "A1", "AA13".  Columns are base-26 with no zero digit, so this is not quite
 * a base conversion: each step takes one off the remaining value first. */
static void zref(zbuf *b, int row, int col)
{
    char t[8];
    int i = 0;
    int c = col;
    do { t[i++] = (char)('A' + c % 26); c = c / 26 - 1; } while (c >= 0 && i < 7);
    while (i) z8(b, (unsigned char)t[--i]);
    zint(b, row + 1);
}

static const char *XERR[] = {
    "#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A"
};
static const int XERRC[] = { 0x00, 0x07, 0x0F, 0x17, 0x1D, 0x24, 0x2A };

static const char *err_text(int code)
{
    int i;
    for (i = 0; i < 7; i++) if (XERRC[i] == code) return XERR[i];
    return "#N/A";
}

/* Excel's built-in number formats, by the ids .xlsx spells them with.  A code
 * that is one of these needs no numFmt element at all. */
static const struct { int id; const char *code; } XBUILTIN[] = {
    { 1, "0" }, { 2, "0.00" }, { 3, "#,##0" }, { 4, "#,##0.00" },
    { 9, "0%" }, { 10, "0.00%" }, { 11, "0.00E+00" }, { 14, "m/d/yy" },
    { 15, "d-mmm-yy" }, { 16, "d-mmm" }, { 17, "mmm-yy" },
    { 18, "h:mm AM/PM" }, { 19, "h:mm:ss AM/PM" }, { 20, "h:mm" },
    { 21, "h:mm:ss" }, { 22, "m/d/yy h:mm" }, { 45, "mm:ss" },
    { 46, "[h]:mm:ss" }, { 47, "mmss.0" }, { 48, "##0.0E+0" }, { 49, "@" }
};
#define NXBUILTIN ((int)(sizeof XBUILTIN / sizeof XBUILTIN[0]))

#define MAXFMT 128

typedef struct {
    const char *code[MAXFMT];    /* the distinct format codes in use         */
    int         id[MAXFMT];      /* the numFmtId each got                    */
    int         n;
    int         next;            /* the next custom id, from 164             */
} xstyles;

/* Returns the CELL XF index for a code: 0 is the General style every cell
 * without a format points at, and each distinct code adds one after it. */
static int style_of(xstyles *st, const char *code)
{
    int i;
    if (!code || !code[0]) return 0;
    for (i = 0; i < st->n; i++)
        if (strcmp(st->code[i], code) == 0) return i + 1;
    if (st->n >= MAXFMT) return 0;
    st->code[st->n] = code;
    for (i = 0; i < NXBUILTIN; i++)
        if (strcmp(XBUILTIN[i].code, code) == 0) break;
    st->id[st->n] = (i < NXBUILTIN) ? XBUILTIN[i].id : st->next++;
    return ++st->n;
}

/* The shared string table.  The model already interned its strings, but it
 * interned them per WORKBOOK and hands them back as pointers, so interning
 * again here is a pointer comparison in the common case. */
#define MAXSST 65536

typedef struct { const char **s; int n, cap; } xsst;

static int sst_of(xsst *t, const char *s)
{
    int i;
    if (!s) s = "";
    for (i = 0; i < t->n; i++) if (t->s[i] == s || strcmp(t->s[i], s) == 0) return i;
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        const char **ns;
        if (nc > MAXSST) return -1;
        ns = (const char **)ud_alloc((unsigned long)nc * sizeof(char *));
        if (!ns) return -1;
        if (t->n) memcpy(ns, t->s, (unsigned long)t->n * sizeof(char *));
        ud_free(t->s);
        t->s = ns; t->cap = nc;
    }
    t->s[t->n] = s;
    return t->n++;
}

/* Cells come out of the model in insertion order.  A worksheet part must be in
 * row-major order, and so must the cells within a row: readers - ours included
 * - are streaming, and a row that arrives after a later one is a row they have
 * already closed. */
static void ordsort(int *idx, const ud_xlsw *w, int s, int n)
{
    int gap = n;
    int done = 0;
    while (!done) {
        int i;
        gap = gap * 10 / 13;
        if (gap < 1) gap = 1;
        done = (gap == 1);
        for (i = 0; i + gap < n; i++) {
            ud_wcellview a, b;
            long ka, kb;
            ud_xlsw_cell_at(w, s, idx[i], &a);
            ud_xlsw_cell_at(w, s, idx[i + gap], &b);
            ka = (long)a.row * UD_XLS_MAXCOL + a.col;
            kb = (long)b.row * UD_XLS_MAXCOL + b.col;
            if (ka > kb) {
                int t = idx[i]; idx[i] = idx[i + gap]; idx[i + gap] = t;
                done = 0;
            }
        }
    }
}

static void sheet_part(zbuf *b, const ud_xlsw *w, int s, xstyles *st, xsst *sst,
                       int *idx, int ncell)
{
    int i, row = -1;

    xml_head(b);
    zs(b, "<worksheet xmlns=\"" ML "spreadsheetml/2006/main\" "
          "xmlns:r=\"" NS_R "\"><sheetData>");
    for (i = 0; i < ncell; i++) {
        ud_wcellview c;
        int style;
        if (!ud_xlsw_cell_at(w, s, idx[i], &c)) continue;
        if (c.row != row) {
            if (row >= 0) zs(b, "</row>");
            zs(b, "<row r=\""); zint(b, c.row + 1); zs(b, "\">");
            row = c.row;
        }
        style = style_of(st, c.fmt);
        zs(b, "<c r=\""); zref(b, c.row, c.col); z8(b, '"');
        if (style) { zs(b, " s=\""); zint(b, style); z8(b, '"'); }
        if (c.kind == UD_XV_STR && !c.formula) zs(b, " t=\"s\"");
        else if (c.kind == UD_XV_STR)          zs(b, " t=\"str\"");
        else if (c.kind == UD_XV_BOOL)         zs(b, " t=\"b\"");
        else if (c.kind == UD_XV_ERR)          zs(b, " t=\"e\"");
        z8(b, '>');
        if (c.formula) {
            /* The model stores the text with its leading '=', which is how a
               user types it; the part stores it without. */
            const char *f = c.formula;
            zs(b, "<f>");
            zxml(b, f[0] == '=' ? f + 1 : f, 0);
            zs(b, "</f>");
        }
        switch (c.kind) {
        case UD_XV_NUM:  zs(b, "<v>"); znum(b, c.num); zs(b, "</v>"); break;
        case UD_XV_BOOL: zs(b, "<v>"); zint(b, c.num != 0); zs(b, "</v>"); break;
        case UD_XV_ERR:  zs(b, "<v>"); zs(b, err_text(c.err)); zs(b, "</v>"); break;
        case UD_XV_STR:
            zs(b, "<v>");
            if (c.formula) zxml(b, c.str ? c.str : "", 0);
            else           zint(b, sst_of(sst, c.str));
            zs(b, "</v>");
            break;
        default: break;                /* a blank cell carrying only a format */
        }
        zs(b, "</c>");
    }
    if (row >= 0) zs(b, "</row>");
    zs(b, "</sheetData>");

    if (ud_xlsw_nmerges(w, s) > 0) {
        int k, nm = ud_xlsw_nmerges(w, s);
        zs(b, "<mergeCells count=\""); zint(b, nm); zs(b, "\">");
        for (k = 0; k < nm; k++) {
            int r0, c0, r1, c1;
            if (!ud_xlsw_merge_at(w, s, k, &r0, &c0, &r1, &c1)) continue;
            zs(b, "<mergeCell ref=\"");
            zref(b, r0, c0); z8(b, ':'); zref(b, r1, c1);
            zs(b, "\"/>");
        }
        zs(b, "</mergeCells>");
    }
    zs(b, "</worksheet>");
}

unsigned char *ud_xlsxw_save(ud_xlsw *w, long *len)
{
    zwrite z;
    zbuf b;
    xstyles st;
    xsst sst;
    int ns, s, i, maxcell = 0;
    int *idx = 0;
    zbuf *sheets = 0;

    if (!w) { ud_set_error("xlsx write: no workbook"); return 0; }
    ns = ud_xlsw_sheets(w);
    if (ns < 1) { ud_set_error("xlsx write: workbook has no sheets"); return 0; }

    memset(&z, 0, sizeof z);
    memset(&st, 0, sizeof st);
    memset(&sst, 0, sizeof sst);
    st.next = 164;

    /* Sheets are serialised BEFORE the styles and shared strings are written,
       because serialising them is what discovers the formats and the strings.
       Hence the staging array: three parts that describe each other cannot be
       written in one pass. */
    for (s = 0; s < ns; s++)
        if (ud_xlsw_ncells(w, s) > maxcell) maxcell = ud_xlsw_ncells(w, s);
    sheets = (zbuf *)ud_alloc((unsigned long)ns * sizeof(zbuf));
    if (maxcell) idx = (int *)ud_alloc((unsigned long)maxcell * sizeof(int));
    if (!sheets || (maxcell && !idx)) {
        ud_free(sheets); ud_free(idx);
        ud_set_error("out of memory (xlsx)");
        return 0;
    }
    memset(sheets, 0, (unsigned long)ns * sizeof(zbuf));
    for (s = 0; s < ns; s++) {
        int nc = ud_xlsw_ncells(w, s);
        for (i = 0; i < nc; i++) idx[i] = i;
        if (nc > 1) ordsort(idx, w, s, nc);
        sheet_part(&sheets[s], w, s, &st, &sst, idx, nc);
    }
    ud_free(idx);

    /* ---- [Content_Types].xml ---- */
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Types xmlns=\"" NS_CT "\">"
           "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
           "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
           "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
           "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
           "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>");
    for (s = 0; s < ns; s++) {
        zs(&b, "<Override PartName=\"/xl/worksheets/sheet");
        zint(&b, s + 1);
        zs(&b, ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>");
    }
    zs(&b, "</Types>");
    zw_part(&z, "[Content_Types].xml", &b);

    root_rels(&z, "xl/workbook.xml");

    /* ---- xl/workbook.xml ---- */
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<workbook xmlns=\"" ML "spreadsheetml/2006/main\" xmlns:r=\"" NS_R "\">");
    if (ud_xlsw_is1904(w))
        zs(&b, "<workbookPr date1904=\"1\"/>");
    zs(&b, "<sheets>");
    for (s = 0; s < ns; s++) {
        zs(&b, "<sheet name=\"");
        zxml(&b, ud_xlsw_sheet_name(w, s), 1);
        zs(&b, "\" sheetId=\""); zint(&b, s + 1);
        zs(&b, "\" r:id=\"rId"); zint(&b, s + 1); zs(&b, "\"/>");
    }
    zs(&b, "</sheets></workbook>");
    zw_part(&z, "xl/workbook.xml", &b);

    /* ---- xl/_rels/workbook.xml.rels ----
       The sheets take rId1..rIdN so a sheet's r:id is its position, and the
       two fixed parts follow.  Nothing requires that, but it makes the
       relationship part readable next to the workbook part. */
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Relationships xmlns=\"" NS_PR "\">");
    for (s = 0; s < ns; s++) {
        zs(&b, "<Relationship Id=\"rId"); zint(&b, s + 1);
        zs(&b, "\" Type=\"" RT "worksheet\" Target=\"worksheets/sheet");
        zint(&b, s + 1); zs(&b, ".xml\"/>");
    }
    zs(&b, "<Relationship Id=\"rId"); zint(&b, ns + 1);
    zs(&b, "\" Type=\"" RT "styles\" Target=\"styles.xml\"/>");
    zs(&b, "<Relationship Id=\"rId"); zint(&b, ns + 2);
    zs(&b, "\" Type=\"" RT "sharedStrings\" Target=\"sharedStrings.xml\"/>");
    zs(&b, "</Relationships>");
    zw_part(&z, "xl/_rels/workbook.xml.rels", &b);

    /* ---- xl/styles.xml ----
       Excel rejects a styles part that omits any of fonts, fills, borders and
       cellStyleXfs, and it insists on two fills specifically: the first two
       fill slots are reserved for "none" and "gray125" whether a file uses
       them or not. */
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<styleSheet xmlns=\"" ML "spreadsheetml/2006/main\">");
    {
        int ncustom = 0;
        for (i = 0; i < st.n; i++) if (st.id[i] >= 164) ncustom++;
        if (ncustom) {
            zs(&b, "<numFmts count=\""); zint(&b, ncustom); zs(&b, "\">");
            for (i = 0; i < st.n; i++) {
                if (st.id[i] < 164) continue;
                zs(&b, "<numFmt numFmtId=\""); zint(&b, st.id[i]);
                zs(&b, "\" formatCode=\""); zxml(&b, st.code[i], 1); zs(&b, "\"/>");
            }
            zs(&b, "</numFmts>");
        }
    }
    zs(&b, "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
           "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
           "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
           "<borders count=\"1\"><border/></borders>"
           "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>");
    zs(&b, "<cellXfs count=\""); zint(&b, st.n + 1); zs(&b, "\">");
    zs(&b, "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>");
    for (i = 0; i < st.n; i++) {
        zs(&b, "<xf numFmtId=\""); zint(&b, st.id[i]);
        zs(&b, "\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>");
    }
    zs(&b, "</cellXfs></styleSheet>");
    zw_part(&z, "xl/styles.xml", &b);

    /* ---- xl/sharedStrings.xml ----
       xml:space="preserve" on every `<t>`: a string that is a single space is
       a thing spreadsheets contain, and without it a conforming parser is
       entitled to throw the space away. */
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<sst xmlns=\"" ML "spreadsheetml/2006/main\" count=\"");
    zint(&b, sst.n); zs(&b, "\" uniqueCount=\""); zint(&b, sst.n); zs(&b, "\">");
    for (i = 0; i < sst.n; i++) {
        zs(&b, "<si><t xml:space=\"preserve\">");
        zxml(&b, sst.s[i], 0);
        zs(&b, "</t></si>");
    }
    zs(&b, "</sst>");
    zw_part(&z, "xl/sharedStrings.xml", &b);
    ud_free(sst.s);

    for (s = 0; s < ns; s++) {
        char name[48];
        zw_name(name, "xl/worksheets/sheet", s + 1, ".xml");
        zw_part(&z, name, &sheets[s]);
    }
    ud_free(sheets);

    return zw_finish(&z, len);
}
