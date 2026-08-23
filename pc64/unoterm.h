/* ===========================================================================
 * UnoDOS/pc64 - unoterm: a VT100/xterm screen emulator.
 *
 * Ported from Portage's TerminalScreen.cs.  Feed it the raw bytes a shell
 * produces and it maintains a grid of cells and a cursor; it draws nothing and
 * knows nothing about a framebuffer, a font or a window.
 *
 * WHY IT IS A SEPARATE FILE FROM ANYTHING THAT DRAWS.  Two reasons, and the
 * second is the real one:
 *
 *   1. It is pure computation, so it is HOST-TESTABLE.  tools/unoterm_test.c
 *      builds this exact file on the dev PC and feeds it recorded escape
 *      streams, which is the only practical way to be sure `vim` and `top`
 *      render, short of running them.
 *   2. There is already a consumer that gets this wrong.  sshapp_ui.c appends
 *      channel bytes to a flat 4 KB scroll buffer, which is fine for
 *      `echo hello` and wrong for anything that moves the cursor: top, vim, a
 *      progress bar, even `ls` with colour, all arrive as escape-sequence
 *      soup.  The bytes were never the problem; having nowhere to put them was.
 *
 * NO ALLOCATION.  The caller hands over one block of memory and unoterm_init
 * carves the screen, the alternate screen and the scrollback ring out of it.
 * A freestanding kernel subsystem that mallocs at every resize is a subsystem
 * that fails at the worst moment; and it makes the host test trivial.
 *
 * DELIBERATELY A SUBSET.  Cursor motion, erase, SGR (16 + 256 + truecolour),
 * insert/delete, scroll regions, the alternate screen, DECSC/DECRC, tabs,
 * bracketed paste, application cursor keys, OSC 0/2 titles and OSC 7 working
 * directory.  NOT: double-width lines, sixels, mouse reporting, or anything
 * needing a font this OS does not have.  Full DEC conformance is not the goal;
 * bash, ls, git, less, vim and top are.
 * ======================================================================== */
#ifndef PC64_UNOTERM_H
#define PC64_UNOTERM_H

/* ---- one cell ------------------------------------------------------------
 * `fg`/`bg` are -1 (the theme's default), 0..255 (the xterm palette) or
 * 0x1000000|rgb (24-bit).  Keeping the three in ONE int rather than a flag
 * plus a value is what stops every consumer from having to remember which
 * combination means what. */
enum {
    UNOTERM_DEFAULT_COLOR = -1,
    UNOTERM_RGB_FLAG      = 0x1000000
};

enum {
    UNOTERM_BOLD      = 1 << 0,
    UNOTERM_INVERSE   = 1 << 1,
    UNOTERM_UNDERLINE = 1 << 2
};

typedef struct {
    unsigned short ch;              /* a Unicode code point, or ' '           */
    short          pad;
    int            fg, bg;
    unsigned char  attr;
    unsigned char  pad2[3];
} unoterm_cell;

/* ---- the emulator -------------------------------------------------------- */
#define UNOTERM_TITLEMAX 64
#define UNOTERM_CWDMAX   128
#define UNOTERM_PARAMS   16

typedef struct unoterm unoterm;

struct unoterm {
    /* geometry */
    int cols, rows;

    /* Storage carved out of the caller's block.  `row[]` is an INDIRECTION
     * table: scrolling rotates indices instead of moving 132 cells per row,
     * which is the difference between a scroll costing a pointer swap and
     * costing a memmove of the whole screen at every line of output. */
    unoterm_cell *store;            /* primary   rows*cols                    */
    unoterm_cell *alt;              /* alternate rows*cols                    */
    unoterm_cell *scroll;           /* scrollback ring, sb_rows*cols          */
    short        *row;              /* rows entries: which store row is line y*/
    short        *arow;             /* the same, for the alternate screen     */
    int           sb_rows, sb_head, sb_count;

    /* cursor and state */
    int cx, cy, saved_cx, saved_cy;
    int scroll_top, scroll_bottom;
    int wrap_next;
    int fg, bg;
    unsigned char attr;

    int state;                      /* the parser's ground/esc/csi/osc        */
    int params[UNOTERM_PARAMS];
    int nparams, param_seen, csi_private;
    char osc[256];
    int  osclen;

    /* UTF-8 reassembly across read boundaries: a multi-byte glyph split over
     * two reads is the normal case on a stream, not an edge case. */
    unsigned utf_acc;
    int      utf_left;

    int alt_screen;
    int cursor_visible;
    int app_cursor_keys;            /* DECCKM: arrows send SS3, not CSI       */
    int bracketed_paste;            /* DECSET 2004                            */
    int dirty;

    char title[UNOTERM_TITLEMAX];
    char cwd[UNOTERM_CWDMAX];       /* from OSC 7, for "open a browser here"  */
};

/* How much memory a given geometry needs.  Ask this, then hand over a block of
 * at least that size - there is no hidden allocation anywhere in this file. */
unsigned long unoterm_memneed(int cols, int rows, int scrollback_rows);

/* Carve `mem` up and reset.  Returns 0 if the block is too small (in which
 * case nothing is written), else 1. */
int unoterm_init(unoterm *t, void *mem, unsigned long memlen,
                 int cols, int rows, int scrollback_rows);

/* Re-flow to a new size, keeping what fits.  The scrollback is preserved. */
void unoterm_resize(unoterm *t, int cols, int rows);

/* Feed raw bytes from the shell.  Any split is safe: the parser and the UTF-8
 * decoder both carry their state across calls. */
void unoterm_feed(unoterm *t, const unsigned char *data, int n);

/* The visible cell at (x, y).  Never NULL for an in-range coordinate; an
 * out-of-range one returns a blank, because a renderer that has to bounds-check
 * every cell grows a bug the first time a resize races a paint. */
const unoterm_cell *unoterm_cell_at(const unoterm *t, int x, int y);

/* One line of scrollback, `back` lines above the top of the screen (1 = the
 * line that scrolled off most recently).  NULL past the end of the ring. */
const unoterm_cell *unoterm_scrollback(const unoterm *t, int back);
int  unoterm_scrollback_count(const unoterm *t);

/* Did anything change since the last ask?  Clears the flag.  A renderer that
 * repaints unconditionally at 60 Hz burns a laptop's battery to draw a shell
 * prompt that has not moved. */
int  unoterm_take_dirty(unoterm *t);

/* Translate a key press into the bytes a terminal would send, honouring
 * application-cursor-key mode.  Returns the byte count written to `out`.
 * `key` is one of the UNOTERM_K_* below, or a Unicode code point for an
 * ordinary character; `ctrl` folds a letter to its control code. */
enum {
    UNOTERM_K_UP = 0x100, UNOTERM_K_DOWN, UNOTERM_K_RIGHT, UNOTERM_K_LEFT,
    UNOTERM_K_HOME, UNOTERM_K_END, UNOTERM_K_PGUP, UNOTERM_K_PGDN,
    UNOTERM_K_INS, UNOTERM_K_DEL, UNOTERM_K_F1, UNOTERM_K_F2, UNOTERM_K_F3,
    UNOTERM_K_F4, UNOTERM_K_F5, UNOTERM_K_F6, UNOTERM_K_F7, UNOTERM_K_F8,
    UNOTERM_K_F9, UNOTERM_K_F10, UNOTERM_K_F11, UNOTERM_K_F12
};
int unoterm_key(const unoterm *t, int key, int ctrl, int alt, char *out, int cap);

/* Wrap pasted text for bracketed paste when the application asked for it, so a
 * multi-line paste is not executed line by line as though it were typed.
 * Returns the bytes written, or the length it WOULD need when `cap` is too
 * small (so a caller can ask first). */
int unoterm_paste(const unoterm *t, const char *text, int len, char *out, int cap);

/* One visible line as plain text, for "copy" and for the host test's
 * assertions.  Trailing blanks are trimmed.  Returns the length. */
int unoterm_line_text(const unoterm *t, int y, char *out, int cap);

#endif
