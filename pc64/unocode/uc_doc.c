/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_doc.c - the text document: buffer, line index, cursors, undo.
 *
 * ONE PRIMITIVE.  Every edit in UnoCode - typing, Backspace, Enter, Tab,
 * comment toggling, moving a line, a Replace All, an extension's
 * TextEditor.edit() - goes through uc_replace_range().  That is what makes
 * undo, dirty tracking, the line index and multi-cursor adjustment correct by
 * construction instead of correct in the places somebody remembered.
 *
 * MULTI-CURSOR is applied LAST CURSOR FIRST.  Editing at the earliest cursor
 * would move every later one, and the classic bug is to fix that up with an
 * offset that is right for insertions and wrong for deletions.  Iterating
 * backwards means no cursor an edit has not yet reached ever moves.
 *
 * THE LINE INDEX is lazy and invalidated wholesale.  A document here is at
 * most 256 KB; rebuilding the offsets of a 6000-line file is one linear pass
 * over memory, which is far cheaper than maintaining incremental correctness
 * through every edit path, and it cannot go subtly wrong.
 * ======================================================================== */
#include "unocode.h"
#include "uc_lsp.h"

static UcDoc  g_doc[UC_DOC_MAX];
static int    g_ndoc, g_active;

/* the clipboard is module-wide: the editor, the terminal and the extension
 * host all cut and paste into the same place, as they do in VS Code */
#define UC_CLIP_CAP (32 * 1024)
static char g_clip[UC_CLIP_CAP];
static int  g_cliplen;

void uc_clip_set(const char *s, int n)
{
    if (n < 0) n = (int)strlen(s);
    if (n > UC_CLIP_CAP - 1) n = UC_CLIP_CAP - 1;
    memcpy(g_clip, s, (unsigned long)n);
    g_clip[n] = 0;
    g_cliplen = n;
}

const char *uc_clip_get(int *n)
{
    if (n) *n = g_cliplen;
    return g_clip;
}

/* ---- document list --------------------------------------------------------- */
int    uc_doc_count(void) { return g_ndoc; }
UcDoc *uc_doc_at(int i) { return (i >= 0 && i < g_ndoc) ? &g_doc[i] : 0; }

/* ---- editor groups (UCD-18) ------------------------------------------------
 * A group is a set of open editors, one of them active, plus the VIEW of that
 * editor: where it is scrolled and where its cursors are.  The view belongs to
 * the group rather than the document because a document has one buffer and can
 * have two viewers, and two viewers of one file that shared a scroll position
 * would not be worth splitting for. */
typedef struct {
    int docs[UC_DOC_MAX];
    int ndoc;
    int active;                       /* doc index, or -1                    */
    int scroll_line, scroll_col;
    UcCursor cur[UC_CURSORS_MAX];
    int ncur;
    int has_view;
} UcGroup;

static UcGroup g_grp[UC_GROUPS];
static int     g_grp_ready;

static int grp_now(void)
{
    int g = (UC.ngroup > 1 && UC.group == 1) ? 1 : 0;
    if (!g_grp_ready) {
        /* first use: every document already open belongs to group 0 */
        int i;
        g_grp_ready = 1;
        g_grp[0].ndoc = 0;
        g_grp[0].active = -1;
        g_grp[1].ndoc = 0;
        g_grp[1].active = -1;
        for (i = 0; i < g_ndoc; i++) g_grp[0].docs[g_grp[0].ndoc++] = i;
        if (g_ndoc) g_grp[0].active = g_active;
    }
    return g;
}

int uc_group_count(int g)
{
    grp_now();
    return (g >= 0 && g < UC_GROUPS) ? g_grp[g].ndoc : 0;
}

int uc_group_doc(int g, int i)
{
    grp_now();
    if (g < 0 || g >= UC_GROUPS || i < 0 || i >= g_grp[g].ndoc) return -1;
    return g_grp[g].docs[i];
}

int uc_group_active(int g)
{
    grp_now();
    if (g < 0 || g >= UC_GROUPS) return -1;
    if (g_grp[g].active >= g_ndoc) g_grp[g].active = g_ndoc ? 0 : -1;
    return g_grp[g].active;
}

static void grp_capture(int g, UcDoc *d)
{
    UcGroup *G;
    int i;
    if (!d || g < 0 || g >= UC_GROUPS) return;
    G = &g_grp[g];
    G->scroll_line = d->scroll_line;
    G->scroll_col = d->scroll_col;
    for (i = 0; i < d->ncur && i < UC_CURSORS_MAX; i++) G->cur[i] = d->cur[i];
    G->ncur = d->ncur;
    G->has_view = 1;
}

static void grp_restore(int g, UcDoc *d)
{
    UcGroup *G;
    int i;
    if (!d || g < 0 || g >= UC_GROUPS) return;
    G = &g_grp[g];
    if (!G->has_view) return;
    d->scroll_line = G->scroll_line;
    d->scroll_col = G->scroll_col;
    for (i = 0; i < G->ncur && i < UC_CURSORS_MAX; i++) d->cur[i] = G->cur[i];
    d->ncur = G->ncur ? G->ncur : 1;
}

/* BORROW group g's view for the length of one paint, then give the document
 * its own back.  This is what lets one file be drawn twice at two scroll
 * positions: the painter is written against UcDoc and there is one of those,
 * so the view is swapped in around the call rather than passed to it. */
static struct { int scroll_line, scroll_col; UcCursor cur[UC_CURSORS_MAX];
                int ncur, held; } g_borrow;

void uc_group_view_push(int g, UcDoc *d)
{
    int i;
    if (!d || g < 0 || g >= UC_GROUPS || g_borrow.held) return;
    g_borrow.scroll_line = d->scroll_line;
    g_borrow.scroll_col = d->scroll_col;
    for (i = 0; i < d->ncur && i < UC_CURSORS_MAX; i++) g_borrow.cur[i] = d->cur[i];
    g_borrow.ncur = d->ncur;
    g_borrow.held = 1;
    grp_restore(g, d);
}

void uc_group_view_pop(UcDoc *d)
{
    int i;
    if (!d || !g_borrow.held) return;
    d->scroll_line = g_borrow.scroll_line;
    d->scroll_col = g_borrow.scroll_col;
    for (i = 0; i < g_borrow.ncur && i < UC_CURSORS_MAX; i++) d->cur[i] = g_borrow.cur[i];
    d->ncur = g_borrow.ncur ? g_borrow.ncur : 1;
    g_borrow.held = 0;
}

void uc_group_show(int g, int doc)
{
    int i;
    grp_now();
    if (g < 0 || g >= UC_GROUPS || doc < 0 || doc >= g_ndoc) return;
    for (i = 0; i < g_grp[g].ndoc; i++) if (g_grp[g].docs[i] == doc) break;
    if (i >= g_grp[g].ndoc && g_grp[g].ndoc < UC_DOC_MAX)
        g_grp[g].docs[g_grp[g].ndoc++] = doc;
    g_grp[g].active = doc;
    g_grp[g].has_view = 0;             /* a fresh view of a fresh editor     */
    if (g == grp_now()) g_active = doc;
}

void uc_group_close(int g, int doc)
{
    int i, k;
    grp_now();
    if (g < 0 || g >= UC_GROUPS) return;
    for (i = 0; i < g_grp[g].ndoc; i++) {
        if (g_grp[g].docs[i] != doc) continue;
        for (k = i; k < g_grp[g].ndoc - 1; k++)
            g_grp[g].docs[k] = g_grp[g].docs[k + 1];
        g_grp[g].ndoc--;
        break;
    }
    if (g_grp[g].active == doc)
        g_grp[g].active = g_grp[g].ndoc ? g_grp[g].docs[0] : -1;
    /* a group with nothing left stops existing, and its space goes back */
    if (g == 1 && !g_grp[1].ndoc && UC.ngroup > 1) {
        UC.ngroup = 1;
        UC.group = 0;
        uc_layout();
    }
}

int uc_group_shows(int doc)
{
    int g, i;
    grp_now();
    for (g = 0; g < UC_GROUPS; g++)
        for (i = 0; i < g_grp[g].ndoc; i++)
            if (g_grp[g].docs[i] == doc) return 1;
    return 0;
}

void uc_group_focus(int g)
{
    int now = grp_now();
    UcDoc *d;
    if (g < 0 || g >= UC_GROUPS || g == now) return;
    if (g == 1 && UC.ngroup < 2) return;
    /* the outgoing group keeps the view it was looking at */
    d = uc_doc_active();
    if (d) grp_capture(now, d);
    UC.group = g;
    if (g_grp[g].active >= 0) g_active = g_grp[g].active;
    d = uc_doc_active();
    if (d) grp_restore(g, d);
    uc_focus(UC_F_EDITOR);
    uc_repaint();
}

void uc_group_split(void)
{
    int now = grp_now();
    int doc = g_grp[now].active;
    int other = now ? 0 : 1;
    if (doc < 0) return;
    if (UC.ngroup < 2) UC.ngroup = 2;
    /* SHOW it in the other group rather than moving it: the same file at two
     * scroll positions is what a split is usually for, and each group's view
     * is its own (see UcGroup). */
    uc_group_show(other, doc);
    UC.group = other;
    g_active = doc;
    /* BOTH groups start from where the editor is right now, and diverge from
     * there.  Without giving the outgoing group a saved view, it has nothing
     * to restore when it paints and simply shows the live document - so
     * scrolling one pane scrolled both, which is the one thing a split is
     * supposed not to do. */
    {
        UcDoc *d = uc_doc_at(doc);
        if (d) { grp_capture(now, d); grp_capture(other, d); }
    }
    uc_layout();
    uc_focus(UC_F_EDITOR);
    uc_repaint();
}

int uc_doc_active_index(void)
{
    int g = grp_now();
    if (!g_ndoc) return -1;
    if (g_grp[g].active >= 0 && g_grp[g].active < g_ndoc) return g_grp[g].active;
    return g_active;
}

UcDoc *uc_doc_active(void)
{
    int i = uc_doc_active_index();
    return (i >= 0) ? &g_doc[i] : 0;
}

void uc_doc_activate(int i)
{
    int g = grp_now();
    if (i >= 0 && i < g_ndoc) {
        g_active = i;
        uc_group_show(g, i);
    }
}

int uc_doc_title(UcDoc *d, char *out, int cap)
{
    if (!d) { uc_scpy(out, "", cap); return 0; }
    uc_scpy(out, d->name[0] ? d->name : "Untitled", cap);
    return 1;
}

int uc_doc_path(UcDoc *d, char *out, int cap)
{
    if (!d) { uc_scpy(out, "", cap); return 0; }
    if (!d->name[0]) { uc_scpy(out, "Untitled", cap); return 0; }
    return uc_path_join(out, cap, d->dir, d->name);
}

/* ---- line index ------------------------------------------------------------ */
/* Every path that changes the text ends here, which makes this the one honest
 * place to count revisions from (UCD-22).  A language server needs a version
 * that rises on every edit and never on anything else; deriving one from the
 * undo stack gets it wrong, because a coalesced keystroke reuses the previous
 * entry and an undo moves the counter backwards. */
static void lines_invalidate(UcDoc *d)
{
    d->lines_ok = 0;
    d->lstate_lines = 0;
    d->rev++;
}

static void lines_build(UcDoc *d)
{
    int i, n = 1, k = 0;
    if (d->lines_ok) return;
    for (i = 0; i < d->len; i++) if (d->text[i] == '\n') n++;
    if (n + 1 > d->loff_cap) {
        int cap = n + 64;
        int *p = (int *)realloc(d->loff, (unsigned long)cap * sizeof(int));
        if (!p) { d->nlines = 1; d->lines_ok = 1; return; }
        d->loff = p;
        d->loff_cap = cap;
    }
    d->loff[k++] = 0;
    for (i = 0; i < d->len; i++)
        if (d->text[i] == '\n' && k < d->loff_cap) d->loff[k++] = i + 1;
    d->nlines = k;
    d->lines_ok = 1;
}

int uc_line_count(UcDoc *d) { if (!d) return 0; lines_build(d); return d->nlines; }

int uc_line_start(UcDoc *d, int line)
{
    lines_build(d);
    if (line <= 0) return 0;
    if (line >= d->nlines) return d->len;
    return d->loff[line];
}

int uc_line_end(UcDoc *d, int line)
{
    int s = uc_line_start(d, line);
    while (s < d->len && d->text[s] != '\n') s++;
    return s;
}

int uc_line_of(UcDoc *d, int off)
{
    int lo = 0, hi;
    lines_build(d);
    hi = d->nlines - 1;
    if (off <= 0) return 0;
    if (off >= d->len) return d->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (d->loff[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Columns are CHARACTERS, not bytes.  These two are a matched pair - a column
 * is read off one line and applied to another for Up/Down, for the goal column
 * and for a second cursor - so counting bytes would put the caret inside a
 * UTF-8 sequence the moment the two lines were not both ASCII.  They are also
 * what the status bar's "Col" and the extension API's Position report, where a
 * byte count would be a lie about a file the user can see. */
int uc_col_of(UcDoc *d, int off)
{
    int s, i, col = 0;
    if (off > d->len) off = d->len;
    if (off < 0) off = 0;
    s = uc_line_start(d, uc_line_of(d, off));
    i = s;
    while (i < off) {
        int cp, len = uc_u8_get(d->text + i, d->len - i, &cp);
        if (len <= 0) break;
        i += len;
        col++;
    }
    return col;
}

int uc_offset_of(UcDoc *d, int line, int col)
{
    int s = uc_line_start(d, line), e = uc_line_end(d, line), i = s;
    while (col > 0 && i < e) {
        int cp, len = uc_u8_get(d->text + i, e - i, &cp);
        if (len <= 0) break;
        i += len;
        col--;
    }
    return i;
}

/* ---- per-line tokenizer state ----------------------------------------------
 * Computed forward from the last known-good line and cached, so a scroll is
 * free and an edit only costs a rescan from the edited line down to whatever
 * is on screen.  The cache is invalidated wholesale by any edit for the same
 * reason the line index is. */
int uc_line_state(UcDoc *d, int line)
{
    int i;
    if (!d) return 0;
    lines_build(d);
    if (line <= 0) return 0;
    if (line >= d->nlines) line = d->nlines - 1;
    if (d->lstate_cap < d->nlines + 1) {
        int cap = d->nlines + 64;
        unsigned short *p = (unsigned short *)realloc(d->lstate,
                                (unsigned long)cap * sizeof(unsigned short));
        if (!p) return 0;
        d->lstate = p;
        d->lstate_cap = cap;
        d->lstate_lines = 0;
    }
    if (line < d->lstate_lines) return d->lstate[line];
    if (d->lstate_lines == 0) { d->lstate[0] = 0; d->lstate_lines = 1; }
    for (i = d->lstate_lines - 1; i < line && i < d->nlines - 1; i++) {
        static short scratch[UC_HL_MAXLINE];
        int s = uc_line_start(d, i), e = uc_line_end(d, i);
        int st = d->lstate[i], out = 0;
        int n = e - s;
        if (n > UC_HL_MAXLINE) n = UC_HL_MAXLINE;
        uc_tokenize(d->lang, d->text + s, n, st, scratch, &out);
        d->lstate[i + 1] = (unsigned short)out;
        d->lstate_lines = i + 2;
    }
    return line < d->lstate_lines ? d->lstate[line] : 0;
}

/* ---- local history --------------------------------------------------------- */
static void base_snapshot(UcDoc *d)
{
    if (d->base) { free(d->base); d->base = 0; d->baselen = 0; }
    d->base = (char *)malloc((unsigned long)d->len + 1);
    if (!d->base) return;
    memcpy(d->base, d->text, (unsigned long)d->len);
    d->base[d->len] = 0;
    d->baselen = d->len;
}

/* Which lines differ from the snapshot taken at open/save.  This is the
 * gutter's "modified" mark - a version-control-shaped signal on a machine with
 * no version control, and honest about being line-for-line rather than a real
 * diff: it compares line N with line N of the base, so an inserted line marks
 * everything below it.  That is the same thing VS Code shows before the first
 * commit, and it costs one string compare. */
int uc_line_changed(UcDoc *d, int line)
{
    /* The painter walks the visible lines in order, so where line N started in
     * the base text is remembered and line N+1 resumes from it.  Without that,
     * finding the base line means counting newlines from byte zero EVERY time,
     * and thirty visible lines cost thirty passes over the file per frame. */
    static UcDoc      *c_doc;
    static const char *c_base;
    static int         c_line, c_off;
    int s, e, n, i, bs = 0, be, bn, bl = 0;
    if (!d || !d->base) return 0;
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    n = e - s;
    if (d == c_doc && d->base == c_base && line >= c_line) { bl = c_line; bs = c_off; }
    for (i = bs; i < d->baselen && bl < line; i++) if (d->base[i] == '\n') { bs = i + 1; bl++; }
    c_doc = d; c_base = d->base; c_line = bl; c_off = bs;
    if (bl < line) return 2;                        /* past the end: added    */
    be = bs;
    while (be < d->baselen && d->base[be] != '\n') be++;
    bn = be - bs;
    if (bn == n && !memcmp(d->text + s, d->base + bs, (unsigned long)n)) return 0;
    return 1;
}

/* ---- undo ------------------------------------------------------------------ */
static void undo_drop(UcEdit *e)
{
    if (e->del) { free(e->del); e->del = 0; }
    if (e->ins) { free(e->ins); e->ins = 0; }
    e->dellen = e->inslen = 0;
}

static void undo_truncate(UcDoc *d)
{
    while (d->nundo > d->undo_at) undo_drop(&d->undo[--d->nundo]);
    /* the redo branch is gone, and so is any hope of returning to a saved
     * state that lived on it */
    if (d->saved_at > d->undo_at) d->saved_at = -1;
}

static UcEdit *undo_push(UcDoc *d)
{
    UcEdit *e;
    undo_truncate(d);
    if (d->nundo >= UC_UNDO_MAX) {
        int i;
        undo_drop(&d->undo[0]);
        for (i = 1; i < d->nundo; i++) d->undo[i - 1] = d->undo[i];
        d->nundo--;
        if (d->saved_at > 0) d->saved_at--; else d->saved_at = -1;
    }
    e = &d->undo[d->nundo++];
    memset(e, 0, sizeof *e);
    d->undo_at = d->nundo;
    return e;
}

/* An explicit group is ONE undo step made of several edits - the multi-cursor
 * case, and the line-range cases (indent, comment, Replace All).  Only the
 * FIRST edit inside it carries group = 0; the rest carry 1, and uc_undo()
 * walks back while the flag is set.  Marking every edit as a continuation -
 * which is what "grouping > 0" alone does - chains the whole session into a
 * single step, and one Ctrl+Z empties the document.  It did. */
void uc_begin_group(UcDoc *d) { if (d && d->grouping++ == 0) d->group_fresh = 1; }
void uc_end_group(UcDoc *d)   { if (d && d->grouping > 0) d->grouping--; }

/* Consecutive typing is coalesced into one undo step, the way every editor
 * people have used does it: a word goes back as a word, not a letter at a
 * time.  The run breaks at a newline, at a change between word and non-word
 * characters, at any caret movement (the previous edit must end exactly where
 * this one starts), and at a length cap so a long paste-by-typing is still
 * undoable in pieces. */
#define UC_COALESCE_MAX 60

static int coalesce_ok(UcDoc *d, int a, int b, const char *s, int n)
{
    UcEdit *p;
    int run = 0, i;
    if (d->grouping > 0) return 0;
    if (a != b || n != 1 || !s || s[0] == '\n') return 0;
    if (d->undo_at <= 0 || d->undo_at != d->nundo) return 0;
    p = &d->undo[d->undo_at - 1];
    if (p->dellen || p->inslen != 1 || !p->ins) return 0;
    if (p->at + 1 != a) return 0;
    if (uc_is_word((unsigned char)p->ins[0]) != uc_is_word((unsigned char)s[0])) return 0;
    for (i = d->undo_at - 1; i >= 0 && d->undo[i].group; i--) run++;
    return run < UC_COALESCE_MAX;
}

/* ---- the one edit primitive ------------------------------------------------ */
/* Where a cursor ends up when [a,b) becomes `n` bytes.  Three cases and the
 * boundaries matter more than the middle:
 *
 *   at or after the END      shifts by the length change
 *   at or after the START    lands at the end of the new text
 *   before the start         does not move
 *
 * The second clause has to include `== a`, and that is the whole point.  With
 * a strict `> a` a plain insertion - where a == b == the caret - matches
 * nothing and the caret does not advance, so every character typed is inserted
 * in front of the last one and the line comes out BACKWARDS.  It did. */
static void cursors_adjust(UcDoc *d, int a, int b, int n)
{
    int i, delta = n - (b - a);
    for (i = 0; i < d->ncur; i++) {
        int *v[2];
        int k;
        v[0] = &d->cur[i].caret;
        v[1] = &d->cur[i].anchor;
        for (k = 0; k < 2; k++) {
            if (*v[k] >= b) *v[k] += delta;
            else if (*v[k] >= a) *v[k] = a + n;
        }
    }
}

void uc_replace_range(UcDoc *d, int a, int b, const char *s, int n)
{
    UcEdit *e;
    int grow;
    if (!d || !d->text || d->readonly) return;
    if (a < 0) a = 0;
    if (b > d->len) b = d->len;
    if (a > b) { int t = a; a = b; b = t; }
    if (n < 0) n = s ? (int)strlen(s) : 0;
    if (a == b && n == 0) return;

    grow = d->len - (b - a) + n;
    if (grow + 1 > d->cap) {
        int cap = grow + 8192;
        char *p;
        if (cap > UC_DOC_CAP) {
            if (grow + 1 > UC_DOC_CAP) { uc_notify("Document size limit reached", UC_SEV_WARN); return; }
            cap = UC_DOC_CAP;
        }
        p = (char *)realloc(d->text, (unsigned long)cap);
        if (!p) return;
        d->text = p;
        d->cap = cap;
    }

    {
        int chain = coalesce_ok(d, a, b, s, n);
        e = undo_push(d);
        e->at = a;
        e->caret_before = d->ncur ? d->cur[0].caret : a;
        if (chain) e->group = 1;
        else if (d->grouping > 0) {
            e->group = (unsigned char)!d->group_fresh;
            d->group_fresh = 0;
        }
    }
    if (b > a) {
        e->del = (char *)malloc((unsigned long)(b - a) + 1);
        if (e->del) {
            memcpy(e->del, d->text + a, (unsigned long)(b - a));
            e->del[b - a] = 0;
            e->dellen = b - a;
        }
    }
    if (n > 0) {
        e->ins = (char *)malloc((unsigned long)n + 1);
        if (e->ins) {
            memcpy(e->ins, s, (unsigned long)n);
            e->ins[n] = 0;
            e->inslen = n;
        }
    }

    memmove(d->text + a + n, d->text + b, (unsigned long)(d->len - b));
    if (n > 0) memcpy(d->text + a, s, (unsigned long)n);
    d->len = grow;
    d->text[d->len] = 0;
    e->caret_after = a + n;

    cursors_adjust(d, a, b, n);
    lines_invalidate(d);
    d->dirty = (d->undo_at != d->saved_at);
}

/* apply an undo record in reverse; does NOT touch the undo stack */
static void apply_reverse(UcDoc *d, UcEdit *e)
{
    int a = e->at, b = a + e->inslen, n = e->dellen;
    int grow = d->len - (b - a) + n;
    if (grow + 1 > d->cap) {
        char *p = (char *)realloc(d->text, (unsigned long)grow + 8192);
        if (!p) return;
        d->text = p;
        d->cap = grow + 8192;
    }
    memmove(d->text + a + n, d->text + b, (unsigned long)(d->len - b));
    if (n > 0 && e->del) memcpy(d->text + a, e->del, (unsigned long)n);
    d->len = grow;
    d->text[d->len] = 0;
    lines_invalidate(d);
}

static void apply_forward(UcDoc *d, UcEdit *e)
{
    int a = e->at, b = a + e->dellen, n = e->inslen;
    int grow = d->len - (b - a) + n;
    if (grow + 1 > d->cap) {
        char *p = (char *)realloc(d->text, (unsigned long)grow + 8192);
        if (!p) return;
        d->text = p;
        d->cap = grow + 8192;
    }
    memmove(d->text + a + n, d->text + b, (unsigned long)(d->len - b));
    if (n > 0 && e->ins) memcpy(d->text + a, e->ins, (unsigned long)n);
    d->len = grow;
    d->text[d->len] = 0;
    lines_invalidate(d);
}

void uc_undo(UcDoc *d)
{
    if (!d || d->undo_at <= 0) return;
    for (;;) {
        UcEdit *e = &d->undo[--d->undo_at];
        apply_reverse(d, e);
        d->ncur = 1;
        d->cur[0].caret = d->cur[0].anchor = e->at;
        if (d->cur[0].caret > d->len) d->cur[0].caret = d->cur[0].anchor = d->len;
        if (!e->group || d->undo_at <= 0) break;
    }
    d->dirty = (d->undo_at != d->saved_at);
}

void uc_redo(UcDoc *d)
{
    if (!d || d->undo_at >= d->nundo) return;
    for (;;) {
        UcEdit *e = &d->undo[d->undo_at++];
        apply_forward(d, e);
        d->ncur = 1;
        d->cur[0].caret = d->cur[0].anchor = e->caret_after;
        if (d->cur[0].caret > d->len) d->cur[0].caret = d->cur[0].anchor = d->len;
        if (d->undo_at >= d->nundo || !d->undo[d->undo_at].group) break;
    }
    d->dirty = (d->undo_at != d->saved_at);
}

/* ---- cursors ---------------------------------------------------------------- */
static void cur_norm(UcDoc *d)
{
    int i, j;
    /* keep them sorted and collapse duplicates: two cursors on one offset are
     * one cursor that types twice, which is never what was meant */
    for (i = 1; i < d->ncur; i++) {
        UcCursor t = d->cur[i];
        for (j = i - 1; j >= 0 && d->cur[j].caret > t.caret; j--) d->cur[j + 1] = d->cur[j];
        d->cur[j + 1] = t;
    }
    for (i = 1; i < d->ncur; ) {
        if (d->cur[i].caret == d->cur[i - 1].caret) {
            int k;
            for (k = i; k < d->ncur - 1; k++) d->cur[k] = d->cur[k + 1];
            d->ncur--;
        } else i++;
    }
    for (i = 0; i < d->ncur; i++) {
        if (d->cur[i].caret < 0) d->cur[i].caret = 0;
        if (d->cur[i].caret > d->len) d->cur[i].caret = d->len;
        if (d->cur[i].anchor < 0) d->cur[i].anchor = 0;
        if (d->cur[i].anchor > d->len) d->cur[i].anchor = d->len;
    }
    if (d->ncur < 1) { d->ncur = 1; d->cur[0].caret = d->cur[0].anchor = 0; }
}

void uc_clear_extra_cursors(UcDoc *d)
{
    if (!d) return;
    if (d->ncur > 1) { d->cur[0] = d->cur[d->ncur - 1]; d->ncur = 1; }
}

void uc_add_cursor(UcDoc *d, int off)
{
    if (!d || d->ncur >= UC_CURSORS_MAX) return;
    d->cur[d->ncur].caret = d->cur[d->ncur].anchor = off;
    d->cur[d->ncur].goal = uc_col_of(d, off);
    d->ncur++;
    cur_norm(d);
}

/* Add a cursor that already HAS a selection.  Ctrl+D needs this: "add the next
 * occurrence" means another cursor with that occurrence selected, and a cursor
 * whose anchor equalled its caret would select nothing (UCD-16). */
void uc_add_cursor_sel(UcDoc *d, int anchor, int caret)
{
    if (!d || d->ncur >= UC_CURSORS_MAX) return;
    d->cur[d->ncur].anchor = anchor;
    d->cur[d->ncur].caret = caret;
    d->cur[d->ncur].goal = uc_col_of(d, caret);
    d->ncur++;
    cur_norm(d);
}

void uc_add_cursor_line(UcDoc *d, int dir)
{
    int i, line, col, nl;
    if (!d || d->ncur >= UC_CURSORS_MAX) return;
    i = dir < 0 ? 0 : d->ncur - 1;
    line = uc_line_of(d, d->cur[i].caret);
    col  = uc_col_of(d, d->cur[i].caret);
    nl = line + dir;
    if (nl < 0 || nl >= uc_line_count(d)) return;
    uc_add_cursor(d, uc_offset_of(d, nl, col));
}

int uc_has_selection(UcDoc *d)
{
    int i;
    if (!d) return 0;
    for (i = 0; i < d->ncur; i++) if (d->cur[i].caret != d->cur[i].anchor) return 1;
    return 0;
}

static void sel_range(const UcCursor *c, int *a, int *b)
{
    if (c->anchor < c->caret) { *a = c->anchor; *b = c->caret; }
    else                      { *a = c->caret;  *b = c->anchor; }
}

int uc_selection_text(UcDoc *d, char *out, int cap)
{
    int i, n = 0;
    if (!d || cap <= 0) return 0;
    for (i = 0; i < d->ncur; i++) {
        int a, b, k;
        sel_range(&d->cur[i], &a, &b);
        if (a == b) continue;
        if (i > 0 && n < cap - 1) out[n++] = '\n';
        for (k = a; k < b && n < cap - 1; k++) out[n++] = d->text[k];
    }
    out[n] = 0;
    return n;
}

void uc_delete_selection(UcDoc *d)
{
    int i;
    if (!d || !uc_has_selection(d)) return;
    uc_begin_group(d);
    for (i = d->ncur - 1; i >= 0; i--) {
        int a, b;
        sel_range(&d->cur[i], &a, &b);
        if (a != b) uc_replace_range(d, a, b, 0, 0);
    }
    uc_end_group(d);
    cur_norm(d);
}

void uc_select_all(UcDoc *d)
{
    if (!d) return;
    d->ncur = 1;
    d->cur[0].anchor = 0;
    d->cur[0].caret = d->len;
}

int uc_word_start(UcDoc *d, int off)
{
    if (off > d->len) off = d->len;
    while (off > 0 && uc_is_word((unsigned char)d->text[off - 1])) off--;
    return off;
}

int uc_word_end(UcDoc *d, int off)
{
    while (off < d->len && uc_is_word((unsigned char)d->text[off])) off++;
    return off;
}

void uc_select_word(UcDoc *d)
{
    int a, b;
    if (!d) return;
    a = uc_word_start(d, d->cur[d->ncur - 1].caret);
    b = uc_word_end(d, d->cur[d->ncur - 1].caret);
    if (a == b) { b = a < d->len ? a + 1 : a; }
    d->cur[d->ncur - 1].anchor = a;
    d->cur[d->ncur - 1].caret = b;
}

void uc_select_line(UcDoc *d)
{
    int line, s, e;
    if (!d) return;
    line = uc_line_of(d, d->cur[d->ncur - 1].caret);
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    if (e < d->len) e++;
    d->cur[d->ncur - 1].anchor = s;
    d->cur[d->ncur - 1].caret = e;
}

/* ---- movement --------------------------------------------------------------- */
void uc_move_to(UcDoc *d, int off, int keep_sel)
{
    if (!d) return;
    if (off < 0) off = 0;
    if (off > d->len) off = d->len;
    d->ncur = 1;
    d->cur[0].caret = off;
    if (!keep_sel) d->cur[0].anchor = off;
    d->cur[0].goal = uc_col_of(d, off);
}

static int step_word(UcDoc *d, int off, int dir)
{
    if (dir > 0) {
        while (off < d->len && !uc_is_word((unsigned char)d->text[off]) && d->text[off] != '\n') off++;
        while (off < d->len && uc_is_word((unsigned char)d->text[off])) off++;
        if (off < d->len && d->text[off] == '\n' && !uc_is_word((unsigned char)d->text[off])) {
            /* stop at the line end rather than swallowing the newline */
        }
    } else {
        while (off > 0 && !uc_is_word((unsigned char)d->text[off - 1]) && d->text[off - 1] != '\n') off--;
        while (off > 0 && uc_is_word((unsigned char)d->text[off - 1])) off--;
        if (off > 0 && d->text[off - 1] == '\n' && off == uc_line_start(d, uc_line_of(d, off)))
            { /* leave the caret at the line start */ }
    }
    return off;
}

void uc_move(UcDoc *d, int dx, int dy, int keep_sel, int by_word)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->ncur; i++) {
        UcCursor *c = &d->cur[i];
        if (dy) {
            int line = uc_line_of(d, c->caret);
            int col  = c->goal > 0 ? c->goal : uc_col_of(d, c->caret);
            int nl = line + dy;
            if (nl < 0) nl = 0;
            if (nl >= uc_line_count(d)) nl = uc_line_count(d) - 1;
            if (c->goal <= 0) c->goal = uc_col_of(d, c->caret);
            col = c->goal;
            c->caret = uc_offset_of(d, nl, col);
        }
        if (dx) {
            if (by_word) c->caret = step_word(d, c->caret, dx);
            else if (!keep_sel && c->caret != c->anchor) {
                int a, b;
                sel_range(c, &a, &b);
                c->caret = dx < 0 ? a : b;
            } else {
                /* one CHARACTER, not one byte: stepping a byte at a time
                 * leaves the caret inside a UTF-8 sequence, where the next
                 * insert splits it and the line becomes mojibake */
                int k = dx < 0 ? -dx : dx;
                while (k--) {
                    if (dx < 0) c->caret = uc_u8_back(d->text, c->caret);
                    else if (c->caret < d->len) {
                        int cp, len = uc_u8_get(d->text + c->caret,
                                                d->len - c->caret, &cp);
                        c->caret += len > 0 ? len : 1;
                    }
                }
            }
            if (c->caret < 0) c->caret = 0;
            if (c->caret > d->len) c->caret = d->len;
            c->goal = uc_col_of(d, c->caret);
        }
        if (!keep_sel) c->anchor = c->caret;
    }
    cur_norm(d);
}

void uc_move_home(UcDoc *d, int keep_sel)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->ncur; i++) {
        UcCursor *c = &d->cur[i];
        int line = uc_line_of(d, c->caret), s = uc_line_start(d, line), t = s;
        /* Home goes to the first non-blank, and to column 0 if already there -
         * the behaviour every editor with indentation eventually adopts */
        while (t < d->len && (d->text[t] == ' ' || d->text[t] == '\t')) t++;
        c->caret = (c->caret == t) ? s : t;
        c->goal = uc_col_of(d, c->caret);
        if (!keep_sel) c->anchor = c->caret;
    }
}

void uc_move_end(UcDoc *d, int keep_sel)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->ncur; i++) {
        UcCursor *c = &d->cur[i];
        c->caret = uc_line_end(d, uc_line_of(d, c->caret));
        c->goal = uc_col_of(d, c->caret);
        if (!keep_sel) c->anchor = c->caret;
    }
}

/* ---- typing ----------------------------------------------------------------- */
/* The file's own indentation wins over the language's, which wins over the
 * setting - unless the user set the setting THEMSELVES, in which case they
 * have said what they want and detection is not an argument against it. */
int uc_doc_tabsize(UcDoc *d)
{
    UcLang *L = d ? uc_lang_at(d->lang) : 0;
    int n = uc_cfg_int("editor.tabSize");
    if (d && d->det_tab > 0 && uc_cfg_bool("editor.detectIndentation") &&
        !uc_cfg_is_user("editor.tabSize"))
        return d->det_tab;
    if (L && L->tabsize > 0 && !uc_cfg_is_user("editor.tabSize")) n = L->tabsize;
    return n > 0 ? n : 4;
}

int uc_doc_spaces(UcDoc *d)
{
    if (d && d->det_spaces >= 0 && uc_cfg_bool("editor.detectIndentation") &&
        !uc_cfg_is_user("editor.insertSpaces"))
        return d->det_spaces;
    return uc_cfg_bool("editor.insertSpaces");
}

/* What this file indents with, from the file itself.
 *
 * Tabs versus spaces is a vote of first characters.  The WIDTH is the most
 * common positive step between one line's indent and the next's, which is the
 * only signal that survives a file whose deepest nesting is four levels of two
 * spaces - counting leading spaces alone would answer 8 there and be wrong at
 * every level. */
void uc_doc_detect_indent(UcDoc *d)
{
    int votes[9], i = 0, tabs = 0, spaces = 0, prev = -1, lines = 0, best = 0, k;
    if (!d) return;
    d->det_spaces = -1;
    d->det_tab = 0;
    for (k = 0; k < 9; k++) votes[k] = 0;
    while (i < d->len && lines < 400) {
        int sp = 0, ch;
        int start = i;
        while (i < d->len && (d->text[i] == ' ' || d->text[i] == '\t')) {
            if (d->text[i] == '\t') { tabs++; sp = -1; break; }
            sp++;
            i++;
        }
        ch = (i < d->len) ? d->text[i] : '\n';
        if (sp >= 0 && ch != '\n' && ch != 0) {          /* a real line       */
            if (sp > 0) spaces++;
            if (prev >= 0) {
                int dl = sp - prev;
                if (dl > 0 && dl <= 8) votes[dl]++;
            }
            prev = sp;
        }
        (void)start;
        while (i < d->len && d->text[i] != '\n') i++;
        if (i < d->len) i++;
        lines++;
    }
    if (!tabs && !spaces) return;                        /* nothing to say    */
    if (tabs > spaces) { d->det_spaces = 0; return; }
    d->det_spaces = 1;
    for (k = 2; k <= 8; k++) if (votes[k] > votes[best]) best = k;
    d->det_tab = (short)(best >= 2 ? best : 0);
}

void uc_insert(UcDoc *d, const char *s, int n)
{
    int i, many;
    if (!d) return;
    if (n < 0) n = (int)strlen(s);
    /* Only GROUP when there is more than one edit to group.  A single-cursor
     * insertion left ungrouped is what lets consecutive typing coalesce. */
    many = d->ncur > 1;
    if (many) uc_begin_group(d);
    for (i = d->ncur - 1; i >= 0; i--) {
        int a, b;
        sel_range(&d->cur[i], &a, &b);
        uc_replace_range(d, a, b, s, n);
    }
    if (many) uc_end_group(d);
    cur_norm(d);
}

/* the closing partner for an auto-closed pair, or 0 */
static int close_of(int c)
{
    switch (c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    default: return 0;
    }
}

void uc_backspace(UcDoc *d)
{
    int i, many;
    if (!d) return;
    if (uc_has_selection(d)) { uc_delete_selection(d); return; }
    many = d->ncur > 1;
    if (many) uc_begin_group(d);
    for (i = d->ncur - 1; i >= 0; i--) {
        /* a whole CHARACTER, not a byte: backspacing one byte out of a
         * multi-byte sequence leaves the rest of it in the buffer as garbage */
        int c = d->cur[i].caret, a = uc_u8_back(d->text, c);
        if (c <= 0) continue;
        /* backspacing through an indent removes the whole tab stop */
        if (d->text[c - 1] == ' ') {
            int line = uc_line_of(d, c), s = uc_line_start(d, line);
            int col = c - s, ts = uc_doc_tabsize(d), k = 0;
            while (s + k < c && d->text[s + k] == ' ') k++;
            if (k == col && col > 0) {
                int back = col % ts ? col % ts : ts;
                a = c - back;
                if (a < s) a = s;
            }
        }
        /* and an auto-closed pair is removed as a pair */
        if (c < d->len && close_of((unsigned char)d->text[c - 1]) == (unsigned char)d->text[c])
            uc_replace_range(d, a, c + 1, 0, 0);
        else
            uc_replace_range(d, a, c, 0, 0);
    }
    if (many) uc_end_group(d);
    cur_norm(d);
}

void uc_del_forward(UcDoc *d)
{
    int i, many;
    if (!d) return;
    if (uc_has_selection(d)) { uc_delete_selection(d); return; }
    many = d->ncur > 1;
    if (many) uc_begin_group(d);
    for (i = d->ncur - 1; i >= 0; i--) {
        int c = d->cur[i].caret, cp, len;
        if (c >= d->len) continue;
        len = uc_u8_get(d->text + c, d->len - c, &cp);   /* a whole character */
        uc_replace_range(d, c, c + (len > 0 ? len : 1), 0, 0);
    }
    if (many) uc_end_group(d);
    cur_norm(d);
}

int uc_indent_of(UcDoc *d, int line, char *pad, int cap)
{
    int s = uc_line_start(d, line), n = 0;
    while (s + n < d->len && (d->text[s + n] == ' ' || d->text[s + n] == '\t') && n < cap - 1) {
        pad[n] = d->text[s + n];
        n++;
    }
    pad[n] = 0;
    return n;
}

void uc_newline(UcDoc *d)
{
    int i;
    if (!d) return;
    uc_begin_group(d);
    for (i = d->ncur - 1; i >= 0; i--) {
        char buf[80];
        int a, b, n = 0, line, extra = 0;
        sel_range(&d->cur[i], &a, &b);
        line = uc_line_of(d, a);
        buf[n++] = '\n';
        if (uc_cfg_bool("editor.autoIndent")) {
            char pad[64];
            int k = uc_indent_of(d, line, pad, sizeof pad), j;
            /* only the indentation that is BEFORE the caret: splitting a line
             * in the middle must not invent leading whitespace */
            int s = uc_line_start(d, line);
            if (s + k > a) k = a - s;
            for (j = 0; j < k && n < (int)sizeof buf - 8; j++) buf[n++] = pad[j];
            /* opening a block indents one more level */
            if (a > 0 && d->text[a - 1] == '{') {
                int ts = uc_doc_tabsize(d), j2;
                if (uc_doc_spaces(d)) { for (j2 = 0; j2 < ts && n < (int)sizeof buf - 4; j2++) buf[n++] = ' '; }
                else if (n < (int)sizeof buf - 2) buf[n++] = '\t';
                if (a < d->len && d->text[a] == '}') extra = 1;
            }
        }
        if (extra) {
            /* "{|}" becomes an opened block with the brace on its own line */
            char tail[80];
            int t = 0, j;
            char pad[64];
            int k = uc_indent_of(d, line, pad, sizeof pad);
            tail[t++] = '\n';
            for (j = 0; j < k && t < (int)sizeof tail - 2; j++) tail[t++] = pad[j];
            uc_replace_range(d, a, b, buf, n);
            {
                int after = d->cur[i].caret;
                uc_replace_range(d, after, after, tail, t);
                d->cur[i].caret = d->cur[i].anchor = after;
            }
        } else {
            uc_replace_range(d, a, b, buf, n);
        }
    }
    uc_end_group(d);
    cur_norm(d);
}

void uc_indent(UcDoc *d, int outdent)
{
    int i, ts, spaces;
    if (!d) return;
    ts = uc_doc_tabsize(d);
    spaces = uc_doc_spaces(d);
    /* Tab with no selection inserts; with a selection it shifts whole lines,
     * which is what makes Shift+Tab mean anything */
    if (!outdent && !uc_has_selection(d)) {
        /* EACH caret gets its OWN tab stop.  One pad computed from cur[0] and
         * inserted everywhere put the other carets at the wrong column, which
         * is exactly the thing multi-cursor exists to get right. */
        uc_begin_group(d);
        for (i = d->ncur - 1; i >= 0; i--) {
            char pad[20];
            int n = 0;
            if (spaces) {
                int col = uc_col_of(d, d->cur[i].caret);
                int k = ts - (col % ts);
                while (n < k && n < (int)sizeof pad - 1) pad[n++] = ' ';
            } else pad[n++] = '\t';
            pad[n] = 0;
            uc_replace_range(d, d->cur[i].caret, d->cur[i].caret, pad, n);
        }
        uc_end_group(d);
        return;
    }
    uc_begin_group(d);
    {
        /* The set of lines ANY cursor touches - not the span from the first to
         * the last.  Two cursors twenty lines apart used to indent everything
         * between them, which is a lot of file to change by accident. */
        int lines[UC_CURSORS_MAX * 8], nl = 0, c, k;
        for (c = 0; c < d->ncur; c++) {
            int a = d->cur[c].anchor, b = d->cur[c].caret, lo, hi, L;
            if (a > b) { int t = a; a = b; b = t; }
            lo = uc_line_of(d, a);
            hi = uc_line_of(d, b);
            /* a selection stopping exactly at a line start has not entered
             * that line, and shifting it would surprise anybody */
            if (hi > lo && b == uc_line_start(d, hi)) hi--;
            for (L = lo; L <= hi && nl < (int)(sizeof lines / sizeof lines[0]); L++) {
                int seen = 0;
                for (k = 0; k < nl; k++) if (lines[k] == L) { seen = 1; break; }
                if (!seen) lines[nl++] = L;
            }
        }
        /* descending: an edit on an earlier line moves every later line start */
        for (i = 1; i < nl; i++) {
            int t = lines[i];
            for (k = i - 1; k >= 0 && lines[k] < t; k--) lines[k + 1] = lines[k];
            lines[k + 1] = t;
        }
        for (i = 0; i < nl; i++) {
            int s = uc_line_start(d, lines[i]);
            if (outdent) {
                int n = 0;
                if (s < d->len && d->text[s] == '\t') n = 1;
                else while (n < ts && s + n < d->len && d->text[s + n] == ' ') n++;
                if (n) uc_replace_range(d, s, s + n, 0, 0);
            } else {
                char pad[20];
                int n = 0;
                if (spaces) { while (n < ts && n < (int)sizeof pad - 1) pad[n++] = ' '; }
                else pad[n++] = '\t';
                uc_replace_range(d, s, s, pad, n);
            }
        }
    }
    uc_end_group(d);
    cur_norm(d);
}

void uc_move_lines(UcDoc *d, int dir)
{
    int line, s, e, ps, pe, n;
    char *tmp;
    if (!d) return;
    line = uc_line_of(d, d->cur[0].caret);
    if (dir < 0 && line == 0) return;
    if (dir > 0 && line >= uc_line_count(d) - 1) return;
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    if (dir < 0) { ps = uc_line_start(d, line - 1); pe = uc_line_end(d, line - 1); }
    else         { ps = uc_line_start(d, line + 1); pe = uc_line_end(d, line + 1); }
    n = e - s;
    tmp = (char *)malloc((unsigned long)n + 2);
    if (!tmp) return;
    memcpy(tmp, d->text + s, (unsigned long)n);
    tmp[n] = 0;
    uc_begin_group(d);
    if (dir < 0) {
        int plen = pe - ps;
        char *prev = (char *)malloc((unsigned long)plen + 2);
        if (prev) {
            memcpy(prev, d->text + ps, (unsigned long)plen);
            prev[plen] = 0;
            uc_replace_range(d, ps, e, 0, 0);
            {
                char *joined = (char *)malloc((unsigned long)(n + plen) + 3);
                if (joined) {
                    int j = 0, k;
                    for (k = 0; k < n; k++) joined[j++] = tmp[k];
                    joined[j++] = '\n';
                    for (k = 0; k < plen; k++) joined[j++] = prev[k];
                    joined[j] = 0;
                    uc_replace_range(d, ps, ps, joined, j);
                    free(joined);
                }
            }
            free(prev);
            uc_move_to(d, uc_offset_of(d, line - 1, uc_col_of(d, d->cur[0].caret)), 0);
        }
    } else {
        int nlen = pe - ps;
        char *next = (char *)malloc((unsigned long)nlen + 2);
        if (next) {
            memcpy(next, d->text + ps, (unsigned long)nlen);
            next[nlen] = 0;
            uc_replace_range(d, s, pe, 0, 0);
            {
                char *joined = (char *)malloc((unsigned long)(n + nlen) + 3);
                if (joined) {
                    int j = 0, k;
                    for (k = 0; k < nlen; k++) joined[j++] = next[k];
                    joined[j++] = '\n';
                    for (k = 0; k < n; k++) joined[j++] = tmp[k];
                    joined[j] = 0;
                    uc_replace_range(d, s, s, joined, j);
                    free(joined);
                }
            }
            free(next);
            uc_move_to(d, uc_offset_of(d, line + 1, uc_col_of(d, d->cur[0].caret)), 0);
        }
    }
    uc_end_group(d);
    free(tmp);
}

void uc_duplicate_lines(UcDoc *d)
{
    int line, s, e, n;
    char *tmp;
    if (!d) return;
    line = uc_line_of(d, d->cur[0].caret);
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    n = e - s;
    tmp = (char *)malloc((unsigned long)n + 2);
    if (!tmp) return;
    tmp[0] = '\n';
    memcpy(tmp + 1, d->text + s, (unsigned long)n);
    uc_replace_range(d, e, e, tmp, n + 1);
    free(tmp);
    uc_move_to(d, uc_offset_of(d, line + 1, uc_col_of(d, d->cur[0].caret)), 0);
}

void uc_delete_line(UcDoc *d)
{
    int line, s, e;
    if (!d) return;
    line = uc_line_of(d, d->cur[0].caret);
    s = uc_line_start(d, line);
    e = uc_line_end(d, line);
    if (e < d->len) e++;
    else if (s > 0) s--;
    uc_replace_range(d, s, e, 0, 0);
    cur_norm(d);
}

void uc_toggle_comment(UcDoc *d)
{
    UcLang *L;
    int lo, hi, i, all_commented = 1, clen;
    if (!d) return;
    L = uc_lang_at(d->lang);
    if (!L || !L->line_comment[0]) return;
    clen = (int)strlen(L->line_comment);
    lo = uc_line_of(d, d->cur[0].anchor < d->cur[0].caret ? d->cur[0].anchor : d->cur[0].caret);
    hi = uc_line_of(d, d->cur[d->ncur-1].anchor > d->cur[d->ncur-1].caret
                        ? d->cur[d->ncur-1].anchor : d->cur[d->ncur-1].caret);
    for (i = lo; i <= hi; i++) {
        int s = uc_line_start(d, i), e = uc_line_end(d, i), t = s;
        while (t < e && (d->text[t] == ' ' || d->text[t] == '\t')) t++;
        if (t == e) continue;                       /* blank lines do not vote */
        if (t + clen > e || strncmp(d->text + t, L->line_comment, (unsigned long)clen)) {
            all_commented = 0;
            break;
        }
    }
    uc_begin_group(d);
    for (i = hi; i >= lo; i--) {
        int s = uc_line_start(d, i), e = uc_line_end(d, i), t = s;
        while (t < e && (d->text[t] == ' ' || d->text[t] == '\t')) t++;
        if (t == e && lo != hi) continue;
        if (all_commented) {
            int k = t + clen;
            if (k <= e && !strncmp(d->text + t, L->line_comment, (unsigned long)clen)) {
                if (k < e && d->text[k] == ' ') k++;
                uc_replace_range(d, t, k, 0, 0);
            }
        } else {
            char pre[8];
            uc_scpy(pre, L->line_comment, sizeof pre);
            uc_scat(pre, " ", sizeof pre);
            uc_replace_range(d, t, t, pre, (int)strlen(pre));
        }
    }
    uc_end_group(d);
    cur_norm(d);
}

int uc_bracket_match(UcDoc *d, int off)
{
    static const char *open = "([{", *close = ")]}";
    int i, depth, dir = 0;
    char want = 0, have = 0;
    const char *p;
    if (!d || off < 0 || off >= d->len) return -1;
    have = d->text[off];
    if ((p = strchr(open, have)) != 0 && have) { dir = 1; want = close[p - open]; }
    else if ((p = strchr(close, have)) != 0 && have) { dir = -1; want = open[p - close]; }
    else return -1;
    depth = 0;
    for (i = off; i >= 0 && i < d->len; i += dir) {
        char c = d->text[i];
        if (c == have) depth++;
        else if (c == want) { if (--depth == 0) return i; }
    }
    return -1;
}

/* ---- open / save ------------------------------------------------------------ */
static void doc_reset(UcDoc *d)
{
    int i;
    for (i = 0; i < d->nundo; i++) undo_drop(&d->undo[i]);
    d->nundo = d->undo_at = 0;
    d->saved_at = 0;
    d->ncur = 1;
    d->cur[0].caret = d->cur[0].anchor = d->cur[0].goal = 0;
    d->scroll_line = d->scroll_col = 0;
    d->dirty = 0;
    d->grouping = 0;
    lines_invalidate(d);
}

static int doc_slot(void)
{
    if (g_ndoc >= UC_DOC_MAX) return -1;
    memset(&g_doc[g_ndoc], 0, sizeof g_doc[0]);
    g_doc[g_ndoc].vol = -1;
    g_doc[g_ndoc].ncur = 1;
    return g_ndoc++;
}

int uc_doc_new(void)
{
    int i = doc_slot();
    UcDoc *d;
    if (i < 0) return -1;
    d = &g_doc[i];
    d->cap = 4096;
    d->text = (char *)malloc((unsigned long)d->cap);
    if (!d->text) { g_ndoc--; return -1; }
    d->text[0] = 0;
    d->len = 0;
    d->lang = 0;
    doc_reset(d);
    base_snapshot(d);
    uc_group_show(grp_now(), i);   /* the group must learn about it too */
    return i;
}

int uc_doc_open(int vol, const char *dir, const char *name)
{
    int i;
    char path[UC_PATH_MAX + UC_NAME_MAX + 2];
    char joined[UC_PATH_MAX + UC_NAME_MAX + 2];
    char leaf[UC_NAME_MAX];
    char *src = 0;
    long len = 0;
    UcDoc *d;
    /* `name` may carry a path ("SDK\SAMPLE.C" from the terminal's `open`, or
     * a manifest's relative path).  Split it, so the tab shows a file name and
     * not a route to one, and so a second open of the same file through a
     * different spelling still finds the document already open. */
    uc_path_join(joined, sizeof joined, dir, name);
    {
        int k, cut = -1;
        for (k = 0; joined[k]; k++) if (joined[k] == '\\' || joined[k] == '/') cut = k;
        if (cut >= 0) {
            uc_scpy(leaf, joined + cut + 1, sizeof leaf);
            joined[cut] = 0;
            dir = joined;
            name = leaf;
        } else {
            dir = "";
            name = joined;
        }
    }
    /* already open? then this is a focus, not a load - and a second copy of a
     * file with unsaved edits would be a way to lose them */
    for (i = 0; i < g_ndoc; i++)
        if (g_doc[i].vol == vol && !uc_ieq(g_doc[i].name, "") &&
            uc_ieq(g_doc[i].name, name) && !strcmp(g_doc[i].dir, dir ? dir : "")) {
            uc_group_show(grp_now(), i);
            return i;
        }
    uc_path_join(path, sizeof path, dir, name);
    if (!uc_read_file(vol, path, &src, &len)) return -1;
    i = doc_slot();
    if (i < 0) { free(src); return -1; }
    d = &g_doc[i];
    /* Normalise CRLF - everything above this line assumes one byte per break -
     * but REMEMBER what was there (UCD-20).  Stripping without recording is
     * what turned a save into a whole-file rewrite. */
    {
        int r, w = 0, crlf = 0, breaks = 0;
        /* COUNT FIRST, then compact.  The compaction writes behind the read
         * cursor, so a counter that looked backwards from w would be reading
         * bytes it had already moved. */
        for (r = 0; r < (int)len; r++) {
            if (src[r] != '\n') continue;
            breaks++;
            if (r > 0 && src[r - 1] == '\r') crlf++;
        }
        for (r = 0; r < (int)len; r++) if (src[r] != '\r') src[w++] = src[r];
        len = w;
        /* "mostly CRLF" rather than "all CRLF": one stray LF in a Windows file
         * should not flip the whole file to Unix endings on the next save. */
        d->eol_crlf = (unsigned char)(breaks > 0 && crlf * 2 >= breaks);
    }
    d->text = src;
    d->len = (int)len;
    d->cap = (int)len + 1;
    d->text[d->len] = 0;
    uc_scpy(d->name, name, sizeof d->name);
    uc_scpy(d->dir, dir ? dir : "", sizeof d->dir);
    d->vol = vol;
    d->exists = 1;
    d->readonly = !uno_fs_writable(vol);
    d->lang = uc_lang_for_file(name);
    uc_doc_detect_indent(d);
    doc_reset(d);
    base_snapshot(d);
    uc_group_show(grp_now(), i);   /* the group must learn about it too */
    uc_api_fire_open(d);
    return i;
}

int uc_doc_save(UcDoc *d)
{
    char path[UC_FULL_MAX];
    int ok, crlf;
    char *out = 0;
    int n;
    if (!d) return 0;
    if (!d->name[0] || d->vol < 0) return 0;
    if (d->readonly || !uno_fs_writable(d->vol)) {
        uc_notify("The volume is read-only", UC_SEV_ERROR);
        return 0;
    }
    if (uc_cfg_bool("files.trimTrailingWhitespace")) {
        int line, nl = uc_line_count(d);
        uc_begin_group(d);
        for (line = nl - 1; line >= 0; line--) {
            int s = uc_line_start(d, line), e = uc_line_end(d, line), t = e;
            while (t > s && (d->text[t-1] == ' ' || d->text[t-1] == '\t')) t--;
            if (t < e) uc_replace_range(d, t, e, 0, 0);
        }
        uc_end_group(d);
    }
    if (uc_cfg_bool("files.insertFinalNewline") && d->len && d->text[d->len-1] != '\n')
        uc_replace_range(d, d->len, d->len, "\n", 1);

    /* The FILE's own endings, unless the user has said what they want.  A
     * setting nobody touched is a default, and a default must not silently
     * rewrite somebody's file from end to end (UCD-20). */
    crlf = uc_cfg_is_user("files.eol") ? !strcmp(uc_cfg_str("files.eol"), "crlf")
                                       : d->eol_crlf;
    uc_path_join(path, sizeof path, d->dir, d->name);
    if (crlf) {
        int i;
        n = 0;
        out = (char *)malloc((unsigned long)d->len * 2 + 2);
        if (!out) return 0;
        for (i = 0; i < d->len; i++) {
            if (d->text[i] == '\n') out[n++] = '\r';
            out[n++] = d->text[i];
        }
    } else {
        out = d->text;
        n = d->len;
    }
    ok = uno_fs_write(d->vol, path, (const unsigned char *)out, n);
    if (crlf) free(out);
    if (ok) {
        d->dirty = 0;
        d->saved_at = d->undo_at;
        d->exists = 1;
        base_snapshot(d);
        uc_api_fire_save(d);
        uc_lsp_did_save(d);
    } else {
        uc_notify("Could not write the file", UC_SEV_ERROR);
    }
    return ok;
}

int uc_doc_save_as(UcDoc *d, int vol, const char *dir, const char *name)
{
    if (!d) return 0;
    uc_scpy(d->name, name, sizeof d->name);
    uc_scpy(d->dir, dir ? dir : "", sizeof d->dir);
    d->vol = vol;
    d->readonly = !uno_fs_writable(vol);
    d->lang = uc_lang_for_file(name);
    d->preview = 0;
    return uc_doc_save(d);
}

int uc_doc_close(int i)
{
    UcDoc *d = uc_doc_at(i);
    int k;
    if (!d) return 0;
    uc_lsp_close_doc(d);          /* before the free, and before the shift */
    for (k = 0; k < d->nundo; k++) undo_drop(&d->undo[k]);
    if (d->text) free(d->text);
    if (d->loff) free(d->loff);
    if (d->lstate) free(d->lstate);
    if (d->base) free(d->base);
    for (k = i; k < g_ndoc - 1; k++) g_doc[k] = g_doc[k + 1];
    g_ndoc--;
    if (g_active >= g_ndoc) g_active = g_ndoc - 1;
    if (g_active < 0) g_active = 0;
    return 1;
}

void uc_doc_free_all(void)
{
    while (g_ndoc > 0) uc_doc_close(g_ndoc - 1);
}
