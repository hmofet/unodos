/* ===========================================================================
 * uoicons.c - the Office 97 toolbar icon atlas (OFFICE97-PLAN §5 phase 6b).
 *
 * 16x16 cells in the 16-colour VGA palette Office 97 drew against, filling
 * the `uoc_set_icons` seam phase 6a left open.  OUR OWN ARTWORK: these are
 * drawn from shape primitives here, in this file, and no Microsoft bitmap was
 * copied or traced.  They are Office-97-*style* - a page, a folder, a
 * diskette, scissors - because that is what the era's icons were, not because
 * any particular one was reproduced.
 *
 * Built procedurally rather than stored as data for two honest reasons: a
 * blob nobody can read is a blob nobody can fix (the same argument that kept
 * a canned STSH out of unodoc's .doc writer), and a 16x16 page-with-a-folded
 * corner is genuinely four rectangles.  The handful of shapes that are not -
 * the letters, the arrows, the scissors - are written as string art, which
 * is editable by anyone who can count.
 * ======================================================================== */
#include "uoicons.h"
#include <string.h>

#define CELL 16
#define COLS 8
/* Enough rows for UOI_COUNT cells, rounded up - the static assert below
 * fails the BUILD rather than the run if an icon is appended past the end,
 * which is how the first version found out (UBSan caught the overrun). */
#define ROWS ((UOI_COUNT + COLS - 1) / COLS)

static fb_px g_atlas[(long)CELL * COLS * CELL * ROWS];
static int   g_built;

/* the VGA sixteen, plus the two off-palette tints Office used for paper */
#define TRN 0u
#define BLK FB_RGB(0x00,0x00,0x00)
#define DGY FB_RGB(0x40,0x40,0x40)
#define GRY FB_RGB(0x80,0x80,0x80)
#define LGY FB_RGB(0xC0,0xC0,0xC0)
#define WHT FB_RGB(0xFF,0xFF,0xFF)
#define NVY FB_RGB(0x00,0x00,0x80)
#define BLU FB_RGB(0x00,0x00,0xFF)
#define CYN FB_RGB(0x00,0xC0,0xC0)
#define GRN FB_RGB(0x00,0x80,0x00)
#define LGR FB_RGB(0x00,0xC0,0x00)
#define RED FB_RGB(0xC0,0x00,0x00)
#define LRD FB_RGB(0xFF,0x40,0x40)
#define YEL FB_RGB(0xFF,0xFF,0x00)
#define TAN FB_RGB(0xFF,0xC8,0x60)
#define BRN FB_RGB(0x80,0x40,0x00)
#define MAG FB_RGB(0xC0,0x00,0xC0)

/* ---- a raster that writes into one cell ----------------------------------- */
static int g_ox, g_oy;
#define AW (CELL * COLS)

static void pset(int x, int y, fb_px c)
{
    long at;
    if (x < 0 || y < 0 || x >= CELL || y >= CELL || c == TRN) return;
    at = (long)(g_oy + y) * AW + g_ox + x;
    if (at < 0 || at >= (long)(sizeof g_atlas / sizeof g_atlas[0])) return;
    g_atlas[at] = c;
}
static void prect(int x, int y, int w, int h, fb_px c)
{ int i, j; for (j = 0; j < h; j++) for (i = 0; i < w; i++) pset(x + i, y + j, c); }
static void pframe(int x, int y, int w, int h, fb_px c)
{
    int i;
    for (i = 0; i < w; i++) { pset(x + i, y, c); pset(x + i, y + h - 1, c); }
    for (i = 0; i < h; i++) { pset(x, y + i, c); pset(x + w - 1, y + i, c); }
}
static void phline(int x, int y, int w, fb_px c) { prect(x, y, w, 1, c); }
static void pvline(int x, int y, int h, fb_px c) { prect(x, y, 1, h, c); }

/* String art, for the shapes that are not rectangles.  One char per pixel:
 * space is transparent, and the rest index the small palette passed in. */
static void art(const char *const *rows, int n, const fb_px *pal, int npal)
{
    int y, x;
    for (y = 0; y < n && y < CELL; y++)
        for (x = 0; rows[y][x] && x < CELL; x++) {
            int c = rows[y][x];
            if (c == ' ') continue;
            c = (c >= '0' && c <= '9') ? c - '0' : (c - 'a' + 10);
            if (c >= 0 && c < npal) pset(x, y, pal[c]);
        }
}

/* ---- the shapes ------------------------------------------------------------ */

/* A sheet of paper with the corner turned down: the base of half the set. */
static void icon_page(int lines)
{
    int i;
    prect(3, 1, 9, 14, WHT);
    pframe(3, 1, 9, 14, GRY);
    prect(9, 1, 3, 3, LGY);            /* the folded corner                 */
    phline(9, 4, 3, GRY);
    pvline(9, 1, 4, GRY);
    for (i = 0; i < lines; i++) phline(5, 6 + i * 2, 5, GRY);
}

static void icon_new(void)   { icon_page(3); }

static void icon_open(void)
{
    prect(1, 5, 12, 8, TAN);           /* the back of the folder            */
    pframe(1, 5, 12, 8, BRN);
    prect(1, 3, 6, 3, TAN);            /* the tab                           */
    pframe(1, 3, 6, 3, BRN);
    prect(3, 8, 12, 6, YEL);           /* the front, opened forward         */
    pframe(3, 8, 12, 6, BRN);
}

static void icon_save(void)
{
    prect(1, 1, 14, 14, NVY);          /* the diskette                      */
    pframe(1, 1, 14, 14, BLK);
    prect(4, 2, 8, 5, LGY);            /* the shutter                       */
    pframe(4, 2, 8, 5, GRY);
    prect(9, 3, 2, 3, DGY);
    prect(3, 9, 10, 6, WHT);           /* the label                         */
    pframe(3, 9, 10, 6, GRY);
}

static void icon_print(void)
{
    prect(3, 2, 10, 4, WHT);           /* the page going in                 */
    pframe(3, 2, 10, 4, GRY);
    prect(1, 6, 14, 5, LGY);           /* the printer body                  */
    pframe(1, 6, 14, 5, DGY);
    prect(11, 7, 2, 2, LGR);           /* the ready lamp                    */
    prect(3, 10, 10, 5, WHT);          /* the page coming out               */
    pframe(3, 10, 10, 5, GRY);
    phline(5, 12, 6, GRY);
}

static void icon_preview(void)
{
    icon_page(2);
    pframe(7, 7, 6, 6, DGY);           /* the magnifier over it             */
    prect(8, 8, 4, 4, CYN);
    pvline(12, 12, 3, DGY);
    pvline(13, 13, 2, DGY);
}

static void icon_zoom(void)
{
    pframe(2, 2, 9, 9, DGY);
    prect(3, 3, 7, 7, CYN);
    prect(9, 10, 3, 3, DGY);
    prect(11, 12, 4, 3, DGY);
}

static void icon_spell(void)
{
    static const char *const rows[] = {
        "                ",
        "  aa    a   aaa ",
        " a  a  a a  a  a",
        " a  a  a a  a  a",
        " aaaa  aaa  aaa ",
        " a  a  a a  a  a",
        " a  a  a a  aaa ",
        "                ",
        "             b  ",
        "            bb  ",
        " b         bb   ",
        " bb       bb    ",
        "  bb     bb     ",
        "   bb   bb      ",
        "    bbbbb       ",
        "     bbb        "
    };
    static const fb_px pal[] = { TRN, BLK, GRN };
    art(rows, 16, pal, 3);
}

static void icon_cut(void)
{
    static const char *const rows[] = {
        "                ",
        "  a          a  ",
        "  aa        aa  ",
        "   aa      aa   ",
        "    aa    aa    ",
        "     aa  aa     ",
        "      aaaa      ",
        "       aa       ",
        "      aaaa      ",
        "     bb  bb     ",
        "    b  b b  b   ",
        "   b    b    b  ",
        "   b    b    b  ",
        "    b  b  b b   ",
        "     bb    bb   ",
        "                "
    };
    static const fb_px pal[] = { TRN, GRY, DGY };
    art(rows, 16, pal, 3);
}

static void icon_copy(void)
{
    prect(1, 1, 8, 11, WHT);
    pframe(1, 1, 8, 11, GRY);
    phline(3, 4, 4, GRY); phline(3, 6, 4, GRY);
    prect(6, 4, 9, 11, WHT);
    pframe(6, 4, 9, 11, DGY);
    phline(8, 7, 5, GRY); phline(8, 9, 5, GRY); phline(8, 11, 5, GRY);
}

static void icon_paste(void)
{
    prect(1, 2, 11, 13, BRN);          /* the clipboard                     */
    pframe(1, 2, 11, 13, DGY);
    prect(4, 1, 5, 3, GRY);            /* the clip                          */
    prect(3, 5, 7, 9, WHT);            /* the sheet on it                   */
    pframe(3, 5, 7, 9, GRY);
    prect(8, 7, 7, 8, WHT);            /* the sheet being pasted            */
    pframe(8, 7, 7, 8, DGY);
}

static void icon_painter(void)
{
    prect(6, 1, 5, 6, YEL);            /* the brush handle                  */
    pframe(6, 1, 5, 6, BRN);
    prect(5, 7, 7, 3, GRY);            /* the ferrule                       */
    pframe(5, 7, 7, 3, DGY);
    prect(6, 10, 5, 5, BLU);           /* the bristles                      */
    phline(4, 14, 9, NVY);
}

static void icon_undo(int mirror)
{
    static const char *const rows[] = {
        "                ",
        "                ",
        "      aaaa      ",
        "    aa    aa    ",
        "   a        a   ",
        "  a          a  ",
        "  a          a  ",
        " a            a ",
        " a              ",
        "aaa             ",
        " aaaaa          ",
        "  aaa           ",
        "   a            ",
        "                ",
        "                ",
        "                "
    };
    static const fb_px pal[] = { TRN, BLU };
    static const fb_px palr[] = { TRN, GRN };
    int y, x;
    if (!mirror) { art(rows, 16, pal, 2); return; }
    for (y = 0; y < 16; y++)
        for (x = 0; rows[y][x]; x++)
            if (rows[y][x] != ' ') pset(15 - x, y, palr[1]);
}

static void icon_link(void)
{
    prect(2, 6, 6, 4, LGY);
    pframe(2, 6, 6, 4, NVY);
    prect(8, 6, 6, 4, LGY);
    pframe(8, 6, 6, 4, NVY);
    phline(7, 7, 3, NVY); phline(7, 8, 3, NVY);
    phline(3, 12, 10, BLU);            /* the underline of a hyperlink      */
}

static void icon_web(void)
{
    pframe(2, 2, 12, 12, NVY);
    prect(3, 3, 10, 10, CYN);
    pvline(8, 3, 10, NVY);
    phline(3, 8, 10, NVY);
    pvline(5, 4, 8, NVY);
    pvline(11, 4, 8, NVY);
    phline(4, 5, 8, NVY);
    phline(4, 11, 8, NVY);
}

static void icon_table(void)
{
    int i;
    pframe(1, 2, 14, 12, DGY);
    prect(2, 3, 12, 3, NVY);           /* the heading row                   */
    for (i = 1; i < 3; i++) phline(2, 3 + i * 3 + 2, 12, GRY);
    for (i = 1; i < 3; i++) pvline(1 + i * 5, 3, 10, GRY);
}

static void icon_columns(void)
{
    int i;
    prect(1, 2, 6, 12, WHT);
    pframe(1, 2, 6, 12, GRY);
    prect(9, 2, 6, 12, WHT);
    pframe(9, 2, 6, 12, GRY);
    for (i = 0; i < 4; i++) { phline(2, 4 + i * 3, 4, GRY); phline(10, 4 + i * 3, 4, GRY); }
}

static void icon_draw(void)
{
    static const char *const rows[] = {
        "            aaa ",
        "           aabaa",
        "          aabbaa",
        "         aabbaa ",
        "        aabbaa  ",
        "       aabbaa   ",
        "      aabbaa    ",
        "     aabbaa     ",
        "    aabbaa      ",
        "   aabbaa       ",
        "  ccbbaa        ",
        " cccbaa         ",
        " ddcaa          ",
        " ddda           ",
        "  dd            ",
        "                "
    };
    static const fb_px pal[] = { TRN, YEL, BRN, TAN, BLK };
    art(rows, 16, pal, 5);
}

static void icon_help(void)
{
    static const char *const rows[] = {
        "                ",
        "     aaaaa      ",
        "   aa     aa    ",
        "  aa       aa   ",
        "  aa       aa   ",
        "           aa   ",
        "          aa    ",
        "        aaa     ",
        "       aa       ",
        "       aa       ",
        "       aa       ",
        "                ",
        "       aa       ",
        "       aa       ",
        "                ",
        "                "
    };
    static const fb_px pal[] = { TRN, NVY };
    art(rows, 16, pal, 2);
}

/* The three character-format letters.  Drawn rather than set in a font,
 * because at 16 pixels a real face is unreadable and Office drew them too. */
static void icon_bold(void)
{
    static const char *const rows[] = {
        "                ",
        "   aaaaaaa      ",
        "   aaaaaaaa     ",
        "   aaa   aaa    ",
        "   aaa   aaa    ",
        "   aaa  aaa     ",
        "   aaaaaaa      ",
        "   aaaaaaa      ",
        "   aaa   aaa    ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "   aaa   aaa    ",
        "   aaaaaaaa     ",
        "   aaaaaaa      ",
        "                ",
        "                "
    };
    static const fb_px pal[] = { TRN, BLK };
    art(rows, 16, pal, 2);
}
static void icon_italic(void)
{
    static const char *const rows[] = {
        "                ",
        "      aaaaaa    ",
        "      aaaaaa    ",
        "        aaa     ",
        "        aaa     ",
        "       aaa      ",
        "       aaa      ",
        "      aaa       ",
        "      aaa       ",
        "     aaa        ",
        "     aaa        ",
        "    aaa         ",
        "   aaaaaa       ",
        "   aaaaaa       ",
        "                ",
        "                "
    };
    static const fb_px pal[] = { TRN, BLK };
    art(rows, 16, pal, 2);
}
static void icon_under(void)
{
    static const char *const rows[] = {
        "                ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "   aaa    aaa   ",
        "    aaa  aaa    ",
        "     aaaaaa     ",
        "      aaaa      ",
        "                ",
        "  aaaaaaaaaa    ",
        "  aaaaaaaaaa    ",
        "                ",
        "                "
    };
    static const fb_px pal[] = { TRN, BLK };
    art(rows, 16, pal, 2);
}

/* Alignment: four line-length patterns.  `mode` 0 left 1 centre 2 right
 * 3 justified - the same shapes Office used, and legible at 16px. */
static void icon_align(int mode)
{
    static const int len[4][5] = {
        { 12, 8, 12, 8, 12 },
        { 12, 8, 12, 8, 12 },
        { 12, 8, 12, 8, 12 },
        { 12, 12, 12, 12, 8 }
    };
    int i;
    for (i = 0; i < 5; i++) {
        int w = len[mode][i], x = 2;
        if (mode == 1) x = 2 + (12 - w) / 2;
        else if (mode == 2) x = 2 + (12 - w);
        phline(x, 2 + i * 3, w, BLK);
    }
}

static void icon_list(int numbered)
{
    int i;
    for (i = 0; i < 4; i++) {
        int y = 2 + i * 4;
        if (numbered) {
            pvline(3, y, 3, BLK);                /* a tally, not a glyph    */
            if (i & 1) pvline(1, y, 3, BLK);
        } else {
            prect(1, y, 3, 3, BLK);
        }
        phline(6, y + 1, 9, BLK);
    }
}

static void icon_indent(int out)
{
    int i;
    for (i = 0; i < 5; i++) phline(6, 2 + i * 3, 9, BLK);
    phline(1, 5, 4, BLK); phline(1, 6, 4, BLK);
    if (out) { pset(1, 4, BLK); pset(1, 7, BLK); pset(2, 3, BLK); pset(2, 8, BLK); }
    else     { pset(4, 4, BLK); pset(4, 7, BLK); pset(3, 3, BLK); pset(3, 8, BLK); }
}

static void icon_borders(void)
{
    int i;
    pframe(1, 2, 14, 12, BLK);
    for (i = 3; i < 14; i += 2) { pset(8, i, GRY); }
    for (i = 2; i < 15; i += 2) { pset(i, 8, GRY); }
}

static void icon_swatch(fb_px c)
{
    prect(2, 2, 12, 8, LGY);
    pframe(2, 2, 12, 8, GRY);
    phline(4, 5, 8, DGY);
    prect(2, 11, 12, 4, c);            /* the colour the button applies     */
    pframe(2, 11, 12, 4, DGY);
}

static void icon_assistant(void)
{
    /* "Uno": a friendly rounded card with two eyes.  Deliberately NOT a
     * paperclip, a dog, a cat or a wizard - the SPEC asks for the frame's
     * fidelity, not for anyone's character. */
    prect(3, 2, 10, 11, YEL);
    pframe(3, 2, 10, 11, BRN);
    pset(3, 2, TRN); pset(12, 2, TRN); pset(3, 12, TRN); pset(12, 12, TRN);
    prect(5, 5, 2, 3, BLK);
    prect(9, 5, 2, 3, BLK);
    phline(6, 10, 4, BRN);
    pset(5, 9, BRN); pset(10, 9, BRN);
}

/* ---- build ----------------------------------------------------------------- */
static void cell(int idx)
{ g_ox = (idx % COLS) * CELL; g_oy = (idx / COLS) * CELL; }

const fb_px *uoc_icons_97(int *pcell, int *pcols, int *pcount)
{
    if (!g_built) {
        memset(g_atlas, 0, sizeof g_atlas);
        cell(UOI_NEW);        icon_new();
        cell(UOI_OPEN);       icon_open();
        cell(UOI_SAVE);       icon_save();
        cell(UOI_PRINT);      icon_print();
        cell(UOI_PREVIEW);    icon_preview();
        cell(UOI_SPELL);      icon_spell();
        cell(UOI_CUT);        icon_cut();
        cell(UOI_COPY);       icon_copy();
        cell(UOI_PASTE);      icon_paste();
        cell(UOI_PAINTER);    icon_painter();
        cell(UOI_UNDO);       icon_undo(0);
        cell(UOI_REDO);       icon_undo(1);
        cell(UOI_LINK);       icon_link();
        cell(UOI_WEB);        icon_web();
        cell(UOI_TABLE);      icon_table();
        cell(UOI_COLUMNS);    icon_columns();
        cell(UOI_DRAW);       icon_draw();
        cell(UOI_ZOOM);       icon_zoom();
        cell(UOI_HELP);       icon_help();
        cell(UOI_BOLD);       icon_bold();
        cell(UOI_ITALIC);     icon_italic();
        cell(UOI_UNDERLINE);  icon_under();
        cell(UOI_ALIGN_L);    icon_align(0);
        cell(UOI_ALIGN_C);    icon_align(1);
        cell(UOI_ALIGN_R);    icon_align(2);
        cell(UOI_JUSTIFY);    icon_align(3);
        cell(UOI_NUMBERING);  icon_list(1);
        cell(UOI_BULLETS);    icon_list(0);
        cell(UOI_INDENT_DEC); icon_indent(1);
        cell(UOI_INDENT_INC); icon_indent(0);
        cell(UOI_BORDERS);    icon_borders();
        cell(UOI_FILLCOLOR);  icon_swatch(YEL);
        cell(UOI_FONTCOLOR);  icon_swatch(RED);
        cell(UOI_HIGHLIGHT);  icon_swatch(LRD);
        cell(UOI_ASSISTANT);  icon_assistant();
        g_built = 1;
    }
    if (pcell)  *pcell  = CELL;
    if (pcols)  *pcols  = COLS;
    if (pcount) *pcount = UOI_COUNT;
    return g_atlas;
}

void uoc_icons_install(void)
{
    int c, n, k;
    const fb_px *a = uoc_icons_97(&c, &k, &n);
    uoc_set_icons(a, c, k, n);
}
