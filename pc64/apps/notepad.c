/* Notepad app module (APP_NOTEPAD).  Separate artifact -> app03.so.
   Text editor with the live status bar (the x86 audit's stale-status rule is
   LAW: status redraws on every edit).  Storage is the portable FAT surface in
   the KernelApi (fat12_read/fat12_write) - the cross-platform path; HFS File
   Manager browsing stays in the full kernel build only. */
#include "uno_mod.h"

#define NBUF   4096
#define NLINE_H 14
#define NTOOL_H 18                      /* toolbar height under the title bar */

static char    gNBuf[NBUF];
static short   gNLen = 0, gNCaret = 0, gNTop = 0;   /* gNTop = first line shown */
static Boolean gNDirty = false;
static char    gNName[16] = "NOTES.TXT";

/* toolbar buttons (x set per-draw from the window's left edge) */
static UiBtn gBSave = {0, 0, 50, 15};
static UiBtn gBOpen = {0, 0, 50, 15};
static UiBtn gBNew  = {0, 0, 44, 15};

static void notepad_toolbar_layout(UnoWin *w)
{
    short x = w->bounds.left + 5, y = w->bounds.top + TBAR_H + 2;
    gBSave.x = x;        gBSave.y = y;
    gBOpen.x = x + 54;   gBOpen.y = y;
    gBNew.x  = x + 108;  gBNew.y  = y;
}

static void notepad_caret_linecol(short *line, short *col)
{
    short i, l = 0, c = 0;
    for (i = 0; i < gNCaret; i++) {
        if (gNBuf[i] == '\r') { l++; c = 0; } else c++;
    }
    *line = l; *col = c;
}

static short notepad_line_start(short line)
{
    short i, l = 0;
    if (line <= 0) return 0;
    for (i = 0; i < gNLen; i++)
        if (gNBuf[i] == '\r' && ++l == line) return i + 1;
    return gNLen;
}

static void notepad_draw(UnoWin *w)
{
    Rect r = w->bounds, ct;
    short ctop = r.top + TBAR_H + NTOOL_H;
    short rows = (r.bottom - ctop - 16) / NLINE_H;
    short x = r.left + 5, y = ctop + 12;
    short line, col, ln = 0, i = 0, drawn = 0;
    char st[80], num[12];

    /* toolbar: clickable Save / Open / New so no key combo is required */
    { Rect tb = r; tb.top += TBAR_H; tb.bottom = ctop; InsetRect(&tb, 1, 0);
      uno_fill(&tb, C_WHITE); }
    notepad_toolbar_layout(w);
    ui_button(&gBSave, gNDirty ? "Save*" : "Save", false);
    ui_button(&gBOpen, "Open", false);
    ui_button(&gBNew,  "New", false);

    ct = r; ct.top = ctop; InsetRect(&ct, 1, 1);
    ct.bottom -= 14;
    uno_fill(&ct, C_BLUE);

    i = notepad_line_start(gNTop);
    ln = gNTop;
    while (i <= gNLen && drawn < rows) {
        short e = i;
        while (e < gNLen && gNBuf[e] != '\r') e++;
        if (e > i) {
            short maxw = r.right - r.left - 12;
            short len = e - i;
            while (len > 0 && TextWidth((Ptr)gNBuf + i, 0, len) > maxw) len--;
            MoveTo(x, y + drawn * NLINE_H);
#if UNO_COLOR
            RGBForeColor(&kPalette[C_WHITE]);
#else
            ForeColor(blackColor);
#endif
            TextMode(srcOr);
            DrawText((Ptr)gNBuf + i, 0, len);
#if UNO_COLOR
            RGBForeColor(&kBlack);
#endif
        }
        notepad_caret_linecol(&line, &col);
        if (line == ln) {
            short cw = TextWidth((Ptr)gNBuf + i, 0, gNCaret - i);
            Rect cr;
            SetRect(&cr, x + cw, y + drawn * NLINE_H - 10, x + cw + 2, y + drawn * NLINE_H + 2);
            uno_fill(&cr, C_CYAN);
        }
        if (e >= gNLen) break;
        i = e + 1; ln++; drawn++;
    }

    {
        Rect sb = r; sb.top = sb.bottom - 14; InsetRect(&sb, 1, 1);
        uno_fill(&sb, C_WHITE);
        notepad_caret_linecol(&line, &col);
        st[0] = 0;
        strcat(st, "Ln "); fmt_u(line + 1, num); strcat(st, num);
        strcat(st, "  Co "); fmt_u(col + 1, num); strcat(st, num);
        strcat(st, "  "); fmt_u(gNLen, num); strcat(st, num); strcat(st, " B");
        if (gNDirty) strcat(st, " *");
        strcat(st, "   (Ctrl-S save)");
        text_at(r.left + 6, r.bottom - 4, st, C_BLUE, C_WHITE, true);
    }
}

static void notepad_scroll_to_caret(UnoWin *w)
{
    short line, col;
    short rows = (w->bounds.bottom - w->bounds.top - TBAR_H - NTOOL_H - 16) / NLINE_H;
    notepad_caret_linecol(&line, &col);
    if (line < gNTop) gNTop = line;
    if (line >= gNTop + rows) gNTop = line - rows + 1;
}

static void notepad_load(void)
{
    long got = fat12_read(gNName, (unsigned char *)gNBuf, NBUF - 1);
    short i;
    gNLen = (short)got; gNCaret = 0; gNTop = 0; gNDirty = false;
    for (i = 0; i < gNLen; i++) if (gNBuf[i] == '\n') gNBuf[i] = '\r';
}

static void notepad_save(void)
{
    if (fat12_write(gNName, (const unsigned char *)gNBuf, gNLen)) {
        gNDirty = false;
        fat12_list();
    }
}

static void notepad_new(void)
{
    gNLen = 0; gNCaret = 0; gNTop = 0; gNDirty = false; gNBuf[0] = 0;
}

static void notepad_opened(void)
{
    if (gNLen == 0) {
        const char *demo = "UnoDOS Notepad\rClick Save / Open / New above -\r"
                           "no key combo needed (Ctrl-S also saves).";
        short n = (short)strlen(demo);
        memcpy(gNBuf, demo, n); gNLen = n; gNCaret = n;
    }
}

static void notepad_click(UnoWin *w, Point p)
{
    if      (ui_hit(&gBSave, p)) notepad_save();
    else if (ui_hit(&gBOpen, p)) notepad_load();
    else if (ui_hit(&gBNew,  p)) notepad_new();
    else return;
    if (w) draw_window(w);
}

static Boolean notepad_key(char ch, short code, Boolean cmd)
{
    UnoWin *w = find_app_window(APP_NOTEPAD);
    if (cmd) {
        if (ch == 's' || ch == 'S') { notepad_save(); if (w) draw_window(w); return true; }
        if (ch == 'o' || ch == 'O') { notepad_load(); if (w) draw_window(w); return true; }
        return false;
    }
    if (code == 0x7B || ch == 0x1C) {                   /* left */
        if (gNCaret > 0) gNCaret--;
    } else if (code == 0x7C || ch == 0x1D) {            /* right */
        if (gNCaret < gNLen) gNCaret++;
    } else if (code == 0x7E || ch == 0x1E) {            /* up */
        short line, col, s;
        notepad_caret_linecol(&line, &col);
        if (line > 0) {
            short prev = notepad_line_start(line - 1);
            short prevLen = notepad_line_start(line) - 1 - prev;
            s = col < prevLen ? col : prevLen;
            gNCaret = prev + (s < 0 ? 0 : s);
        }
    } else if (code == 0x7D || ch == 0x1F) {            /* down */
        short line, col;
        notepad_caret_linecol(&line, &col);
        {
            short next = notepad_line_start(line + 1);
            if (next <= gNLen) {
                short e = next, nl;
                while (e < gNLen && gNBuf[e] != '\r') e++;
                nl = e - next;
                gNCaret = next + (col < nl ? col : nl);
            }
        }
    } else if (ch == 0x08 || ch == 0x7F) {              /* backspace/del */
        if (gNCaret > 0) {
            memmove(gNBuf + gNCaret - 1, gNBuf + gNCaret, gNLen - gNCaret);
            gNCaret--; gNLen--; gNDirty = true;
        }
    } else if (ch == 0x0D || ch == 0x03 || ch >= 32) {  /* insert */
        if (gNLen < NBUF - 1) {
            char c = (ch == 0x03) ? 0x0D : ch;
            memmove(gNBuf + gNCaret + 1, gNBuf + gNCaret, gNLen - gNCaret);
            gNBuf[gNCaret++] = c; gNLen++; gNDirty = true;
        }
    } else {
        return false;
    }
    if (w) { notepad_scroll_to_caret(w); draw_window(w); }
    return true;
}

static const AppInterface kIface = {
    notepad_draw, notepad_key, notepad_click, 0, notepad_opened, 0,
    "Notepad", { 56, 34, 484, 320 }
};
const AppInterface *uno_app_main(const KernelApi *k){ gK = k; return &kIface; }
