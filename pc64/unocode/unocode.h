/* ===========================================================================
 * UnoCode - the UnoDOS pc64 code editor, in the shape of Visual Studio Code.
 *
 * This header is the SUBSYSTEM-INTERNAL contract: the types every uc_*.c file
 * shares.  Nothing outside pc64/unocode/ may include it.  The only surface
 * UnoCode presents to the rest of the OS is the one every app presents - a
 * unoui-CLASS module (APPS\UNOCODE.UNO) reached through uno_uuiapp.h - plus
 * the on-disk formats documented in UNOCODE.md, which are what extensions,
 * themes and settings are actually written against.
 *
 * The workbench is drawn into ONE UI_CANVAS.  That is deliberate: VS Code's
 * chrome (activity bar, side bar, tab strip, minimap, panel, status bar,
 * overlay palettes) is not a stack of toolkit widgets, and building it out of
 * unoui widgets would have meant either a widget explosion or a set of theme
 * painters that only one app uses.  unoui still owns the WINDOW; we own the
 * pixels inside it, exactly as Studio, the browser and Paint do.
 *
 * Layering, strictly bottom-up (a file may only call downward):
 *
 *      uc_json   uc_rx                    (no UnoCode types; pure data)
 *      uc_theme  uc_cfg  uc_lang          (models built from JSON)
 *      uc_doc                             (the text buffer + undo + cursors)
 *      uc_edit   uc_view  uc_term         (painting + input over the models)
 *      uc_cmd                             (commands, keybindings, palette)
 *      uc_ext    uc_api                   (extension host over unojs)
 *      uc_main                            (module entry, layout, routing)
 * ======================================================================== */
#ifndef UNOCODE_H
#define UNOCODE_H

#include "unoui.h"
#include "unoui_theme.h"
#include "fb.h"
#include "fat.h"      /* unofs's richer listing: uno_fat_list_ex + is_dir */

/* ---- kernel imports ------------------------------------------------------
 * Every one of these is a name in pc64_modload.c's kExports[]; build.sh fails
 * the build if we call anything that is not.  Declared here once so no uc_*.c
 * re-declares them subtly differently. */
void  fb_fill_rect(int x, int y, int w, int h, fb_px c);
void  fb_hline(int x, int y, int w, fb_px c);
void  fb_vline(int x, int y, int h, fb_px c);
void  fb_frame_rect(int x, int y, int w, int h, fb_px c);
void  fb_pixel(int x, int y, fb_px c);
void  fb_blend_rect(int x, int y, int w, int h, fb_px c, int a);
void  fb_round_rect(int x, int y, int w, int h, int rad, fb_px c);
void  fb_set_clip(int x, int y, int w, int h);
void  fb_reset_clip(void);
int   fb_text(int x, int y, const char *s, fb_px fg, long bg);
int   fb_text_w(const char *s);
int   fb_text_h(void);
int   fb_width(void);
int   fb_height(void);
int   uno_font_draw_styled(int slot, int px, int style, int x, int y,
                           const char *s, fb_px fg, long bg);
int   uno_font_draw_mono(int slot, int px, int style, int x, int y,
                         const char *s, int cellw, fb_px fg);
int   uno_font_text_w_styled(int slot, int px, int style, const char *s);
int   uno_font_height_px(int slot, int px);
int   uno_font_baseline_px(int slot, int px);

int   uno_fs_volumes(void);
const char *uno_fs_volume_name(int vol);
int   uno_fs_list_begin(int vol);
int   uno_fs_list_get(int vol, int idx, char *name, int max);
int   uno_fs_list_dir(int vol, const char *dir, char (*names)[16], int maxn);
long  uno_fs_read(int vol, const char *name, unsigned char *buf, long max);
long  uno_fs_size(int vol, const char *name);
int   uno_fs_write(int vol, const char *name, const unsigned char *buf, long len);
int   uno_fs_writable(int vol);
int   uno_fs_isdir(int vol, const char *path);
int   uno_fs_mkdir(int vol, const char *path);
int   uno_fs_kind(int vol);
int   uno_fs_pref_vol(void);
int   uno_fs_fat_index(int vol);
int   uno_fat_delete(int vol, const char *path);

void  pc64_shell_dirty(void);
const struct unoui_theme *pc64_shell_theme(void);
int   pc64_shell_font_mono(void);
int   pc64_shell_run_user(int vol, const char *path);
/* The shell's NATIVE file picker, if it has one.  A hosted build (UnoCode
 * Desktop) opens the OS dialog here, because users expect their own file
 * manager's places and bookmarks rather than a list this module invented, and
 * because only the shell can re-root the workspace volume afterwards.  Answers
 * 0 when there is no such thing - which is the case on pc64, where the
 * in-editor quick-open remains the way to open a file. */
int   pc64_shell_pick(int want_folder, int *vol, char *dir, int dcap,
                      char *name, int ncap);
void  pc64_browser_open_path(const char *path);
const char *pc64_shell_py_error(void);
int   pc64_shell_workarea_w(void);
int   pc64_shell_workarea_h(void);
unsigned long uno_dbg_uptime_ms(void);

void *malloc(unsigned long n);
void *realloc(void *p, unsigned long n);
void  free(void *p);
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);
int   snprintf(char *b, unsigned long n, const char *f, ...);
long  strtol(const char *s, char **e, int base);

/* ======================================================================== *
 * uc_json.c - JSON with comments (JSONC), the format every UnoCode config
 * file is written in.  VS Code's settings.json, keybindings.json, themes and
 * package.json all allow comments and trailing commas; a parser that rejected
 * them would reject the files users actually write.
 *
 * Values are bump-allocated out of ONE arena per parse, so a whole document is
 * freed with a single uc_json_free() and no node ever needs an owner.
 * ======================================================================== */
enum { UJ_NULL = 0, UJ_BOOL, UJ_NUM, UJ_STR, UJ_ARR, UJ_OBJ };

typedef struct UcJson {
    unsigned char   type;
    int             bval;      /* UJ_BOOL */
    double          num;       /* UJ_NUM  */
    char           *str;       /* UJ_STR (NUL-terminated, in the arena)     */
    char           *key;       /* member name when the parent is UJ_OBJ     */
    struct UcJson  *child;     /* first element/member                      */
    struct UcJson  *next;      /* next sibling                              */
    int             n;         /* element/member count                      */
} UcJson;

/* Parse `src` (len < 0 = strlen).  Returns the root, or NULL with a one-line
 * reason in `err`.  Free with uc_json_free(root) - never free a child. */
UcJson *uc_json_parse(const char *src, int len, char *err, int errcap);
void    uc_json_free(UcJson *root);

UcJson     *uc_json_member(const UcJson *o, const char *key);  /* NULL if absent */
UcJson     *uc_json_at(const UcJson *a, int i);
const char *uc_json_str (const UcJson *o, const char *key, const char *dflt);
double      uc_json_num (const UcJson *o, const char *key, double dflt);
int         uc_json_bool(const UcJson *o, const char *key, int dflt);
/* "editor.minimap.enabled" walks nested objects AND flat dotted keys, because
 * VS Code's settings.json accepts either spelling for the same setting. */
UcJson     *uc_json_path(const UcJson *root, const char *dotted);

/* A JSON string escaper for the writers (settings.json, keybindings.json). */
int uc_json_esc(char *out, int cap, const char *s);

/* ======================================================================== *
 * uc_rx.c - the regular-expression engine.
 *
 * Grammars are the reason this exists: a TextMate grammar is a tree of
 * regexes, and a syntax highlighter driven by hand-written scanners cannot be
 * EXTENDED by a file dropped on the disk, which is the whole point.  Find &
 * Replace gets regex mode for free.
 *
 * Backtracking matcher.  Supported: literals, `.`, character classes with
 * ranges and negation, the \w \W \d \D \s \S \b \B escapes, anchors, the
 * greedy and non-greedy quantifiers including {n,m}, alternation, capture and
 * non-capture groups.  NOT supported: backreferences and lookaround -
 * uc_rx_compile() reports those as errors rather than mis-matching silently.
 * ======================================================================== */
#define UC_RX_CAPS 10

typedef struct UcRx UcRx;

UcRx *uc_rx_compile(const char *pat, int icase, char *err, int errcap);
void  uc_rx_free(UcRx *rx);
/* Search s[from..len) for the leftmost match.  Returns 1 and fills
 * caps[0..2*UC_RX_CAPS) with start/end pairs (group 0 = the whole match, -1 =
 * a group that did not participate), or 0 for no match.  `bol` says whether
 * index `from` is a line start, so `^` behaves. */
int   uc_rx_exec(UcRx *rx, const char *s, int len, int from, int bol, int *caps);
int   uc_rx_ngroups(const UcRx *rx);

/* ======================================================================== *
 * uc_theme.c - colour themes.
 *
 * A UnoCode theme file IS a VS Code colour theme: {"name","type","colors",
 * "tokenColors"}.  `colors` is a flat map of workbench colour keys; the ones
 * we paint with are the UC_C_* enum below and an unknown key is ignored (VS
 * Code has several hundred and no theme sets them all - a theme that only
 * knows about a future key must still load).  Anything a theme leaves unset is
 * derived from its `type`, so a three-key theme still renders.
 * ======================================================================== */
enum {
    UC_C_FG = 0, UC_C_ERROR_FG, UC_C_WARN_FG, UC_C_INFO_FG, UC_C_FOCUS_BORDER,
    UC_C_EDITOR_BG, UC_C_EDITOR_FG, UC_C_LINENO, UC_C_LINENO_ACTIVE,
    UC_C_SELECTION, UC_C_SELECTION_HL, UC_C_LINE_HL, UC_C_CURSOR,
    UC_C_WHITESPACE, UC_C_INDENT_GUIDE, UC_C_INDENT_GUIDE_ACTIVE,
    UC_C_BRACKET_MATCH, UC_C_FIND_MATCH, UC_C_FIND_MATCH_HL, UC_C_RULER,
    UC_C_SIDEBAR_BG, UC_C_SIDEBAR_FG, UC_C_SIDEBAR_TITLE, UC_C_SIDEBAR_SECT,
    UC_C_SIDEBAR_BORDER,
    UC_C_ACTIVITY_BG, UC_C_ACTIVITY_FG, UC_C_ACTIVITY_DIM,
    UC_C_ACTIVITY_BORDER, UC_C_BADGE_BG, UC_C_BADGE_FG,
    UC_C_STATUS_BG, UC_C_STATUS_FG, UC_C_STATUS_NOFOLDER, UC_C_STATUS_DEBUG,
    UC_C_STATUS_HOVER,
    UC_C_TITLE_BG, UC_C_TITLE_FG,
    UC_C_TABS_BG, UC_C_TAB_ACTIVE_BG, UC_C_TAB_ACTIVE_FG,
    UC_C_TAB_INACTIVE_BG, UC_C_TAB_INACTIVE_FG, UC_C_TAB_BORDER,
    UC_C_TAB_ACTIVE_TOP, UC_C_TAB_MODIFIED, UC_C_BREADCRUMB_FG,
    UC_C_PANEL_BG, UC_C_PANEL_BORDER, UC_C_PANEL_TITLE, UC_C_PANEL_TITLE_DIM,
    UC_C_TERM_BG, UC_C_TERM_FG, UC_C_TERM_RED, UC_C_TERM_GREEN,
    UC_C_TERM_YELLOW, UC_C_TERM_BLUE, UC_C_TERM_CYAN,
    UC_C_LIST_SEL_BG, UC_C_LIST_SEL_FG, UC_C_LIST_HOVER_BG,
    UC_C_LIST_INACTIVE_BG, UC_C_LIST_HIGHLIGHT,
    UC_C_INPUT_BG, UC_C_INPUT_FG, UC_C_INPUT_BORDER, UC_C_INPUT_PLACEHOLDER,
    UC_C_WIDGET_BG, UC_C_WIDGET_BORDER, UC_C_WIDGET_SHADOW,
    UC_C_QUICK_BG, UC_C_QUICK_SEL_BG,
    UC_C_SUGGEST_BG, UC_C_SUGGEST_SEL_BG, UC_C_SUGGEST_BORDER,
    UC_C_SCROLL_SLIDER, UC_C_SCROLL_HOVER,
    UC_C_BUTTON_BG, UC_C_BUTTON_FG, UC_C_BUTTON_HOVER, UC_C_MINIMAP_SLIDER,
    UC_C_GUTTER_ADDED, UC_C_GUTTER_MODIFIED, UC_C_GUTTER_DELETED,
    UC_C_GIT_MODIFIED, UC_C_GIT_ADDED, UC_C_GIT_DELETED,
    UC_C_NOTIF_BG, UC_C_NOTIF_FG, UC_C_NOTIF_BORDER,
    UC_C_N
};

/* fontStyle bits in a tokenColor's settings */
#define UC_FS_BOLD      1
#define UC_FS_ITALIC    2
#define UC_FS_UNDERLINE 4

/* Sized to keep this module's .bss modest: it loads into the shared 4 MB
 * module arena, which never frees, so every static array here is memory no
 * other app can have for as long as the machine is up. */
#define UC_TOK_SCOPES 72
#define UC_THEMES_MAX 20

typedef struct {
    char  scope[52];      /* "keyword.control" - matched by dotted prefix   */
    fb_px fg;
    unsigned char has_fg, style;
} UcTokenRule;

typedef struct UcTheme {
    char  name[40];       /* the label the theme picker shows               */
    char  file[72];       /* "" for a built-in                              */
    signed char vol;      /* -1 = built-in                                  */
    unsigned char dark;   /* "type": "dark" | "light" | "hc-black"          */
    unsigned char hc;     /* high contrast                                  */
    unsigned char builtin;
    unsigned char loaded; /* the JSON has been read into c[]/tok[]          */
    fb_px c[UC_C_N];
    UcTokenRule tok[UC_TOK_SCOPES];
    int   ntok;
    int   ext;            /* contributing extension index, -1 = built-in    */
} UcTheme;

void      uc_theme_init(void);                 /* register the built-ins     */
int       uc_theme_count(void);
UcTheme  *uc_theme_at(int i);
UcTheme  *uc_theme_active(void);
int       uc_theme_find(const char *name);     /* by label; -1 if unknown    */
int       uc_theme_select(const char *name);   /* 1 if it changed            */
/* Register a theme contributed by an extension.  Its JSON is loaded lazily,
 * on first selection, so twenty installed themes cost twenty names. */
int       uc_theme_register(const char *label, int vol, const char *path,
                            int dark, int ext);
fb_px     uc_col(int idx);                     /* active theme's colour      */
/* Resolve a token scope ("keyword.control.c") to a colour + style, using
 * longest-dotted-prefix wins - the rule TextMate themes are written to. */
fb_px     uc_tok_color(const char *scope, int *style);
/* Blend `a` over `b` at alpha (0..255) - themes give selection colours as
 * eight-digit hexes and we have no alpha buffer to composite them into. */
fb_px     uc_blend(fb_px a, fb_px b, int alpha);
int       uc_parse_hex_color(const char *s, fb_px *out, int *alpha);
const char *uc_color_key(int idx);              /* the VS Code key name      */

/* ======================================================================== *
 * uc_cfg.c - settings.
 *
 * Three layers, later wins: built-in defaults -> user settings.json ->
 * workspace settings.  Every setting is DECLARED in a table with a type and a
 * default, which is what makes the Settings editor and getConfiguration()
 * possible without either of them hard-coding a list.
 * ======================================================================== */
enum { UC_T_BOOL = 0, UC_T_INT, UC_T_STR, UC_T_ENUM };

typedef struct {
    const char *key;          /* "editor.fontSize"                          */
    unsigned char type;
    const char *dflt;
    const char *enums;        /* "off|on|bounded" for UC_T_ENUM             */
    const char *desc;
    int   lo, hi;             /* UC_T_INT range                             */
} UcSettingDef;

void        uc_cfg_init(void);              /* defaults + load user file    */
int         uc_cfg_load(void);              /* re-read from disk            */
int         uc_cfg_save(void);              /* write user settings.json     */
int         uc_cfg_int (const char *key);
int         uc_cfg_bool(const char *key);
const char *uc_cfg_str (const char *key);
/* Set + persist.  `val` is the JSON text of the value: "true", "14", "\"x\"". */
int         uc_cfg_set (const char *key, const char *val);
int         uc_cfg_count(void);
const UcSettingDef *uc_cfg_def(int i);
const UcSettingDef *uc_cfg_find(const char *key);
int         uc_cfg_is_user(const char *key);   /* overridden by the user?   */
/* the raw JSON node for a key, so an extension's own contributed settings -
 * which have no UcSettingDef - are still readable. */
UcJson     *uc_cfg_raw(const char *key);
const char *uc_cfg_path(void);                 /* "UNOCODE\SETTINGS.JSN"    */
int         uc_cfg_vol(void);
/* the config directory on the state volume, created on demand */
int         uc_cfg_dir(char *out, int cap);

/* ======================================================================== *
 * uc_lang.c - languages, grammars and the tokenizer.
 * ======================================================================== */
#define UC_LANG_MAX      16
#define UC_LANG_EXTS     6
#define UC_GRAM_PATTERNS 96
#define UC_GRAM_DEPTH    10

typedef struct UcPattern {
    char  name[52];             /* scope assigned to the whole match        */
    char  cap[UC_RX_CAPS][40];  /* scope per capture group, "" = none       */
    UcRx *match;                /* one-line rule                            */
    UcRx *begin, *end;          /* multi-line rule                          */
    char  content[52];          /* contentName for a begin/end rule         */
    short sub, nsub;            /* slice of the grammar's pattern pool      */
    unsigned char self;         /* $self include: recurse to the top level  */
} UcPattern;

typedef struct UcGrammar {
    char       scope[52];       /* "source.c"                               */
    UcPattern *pat;
    int        npat, pcap;
    short      top, ntop;       /* the root pattern list                    */
    int        ok;
} UcGrammar;

typedef struct UcLang {
    char id[16];                /* "c", "python", "json", "plaintext"       */
    char name[24];              /* "C"                                      */
    char ext[UC_LANG_EXTS][8];  /* ".C" - upper case, as FAT reports them   */
    int  next;
    char line_comment[4];
    char block_open[4], block_close[4];
    const char *keywords;       /* space-separated, for word suggestions    */
    UcGrammar  *gram;
    int   tabsize;
    int   ext_index;            /* contributing extension, -1 = built-in    */
} UcLang;

/* The longest line the highlighter will colour.  Past this the line is drawn
 * in the default foreground: a 40 KB minified line is not worth stalling a
 * repaint over, and VS Code makes the same trade. */
#define UC_HL_MAXLINE 2048

void      uc_lang_init(void);
int       uc_lang_count(void);
UcLang   *uc_lang_at(int i);
int       uc_lang_for_file(const char *name);      /* index, 0 = plaintext  */
int       uc_lang_by_id(const char *id);
int       uc_lang_register(const UcLang *proto);   /* extension-contributed */
/* Load a TextMate-lite grammar file and attach it to language `lang`. */
int       uc_lang_load_grammar(int lang, int vol, const char *path);

/* Colour one line.  `scope_out` receives ONE INTERNED SCOPE ID PER CHARACTER
 * rather than a list of token runs - which is the shape the painter wants, and
 * the shape that makes overlapping rules (a capture scope inside its parent's
 * match) fall out for free instead of needing a merge pass.
 *
 * `state_in` is the tokenizer state at the START of the line (0 at the top of
 * the file) and *state_out gets the state after it; that is what makes a block
 * comment survive a scroll.  Returns 1 if the line was coloured, 0 if it was
 * left at the default (no grammar, or longer than UC_HL_MAXLINE). */
int       uc_tokenize(int lang, const char *line, int len, int state_in,
                      short *scope_out, int *state_out);
const char *uc_scope_name(int scope_id);
int       uc_scope_id(const char *name);

/* ======================================================================== *
 * uc_doc.c - the text document.
 * ======================================================================== */
#define UC_DOC_MAX      12          /* open editors                         */
#define UC_DOC_CAP      (256*1024)  /* one document's ceiling               */
#define UC_CURSORS_MAX  32
#define UC_UNDO_MAX     120
#define UC_PATH_MAX     72

typedef struct { int caret, anchor, goal; } UcCursor;

typedef struct {
    int   at;                 /* offset the edit applied at                 */
    int   dellen, inslen;
    char *del, *ins;          /* malloc'd text                              */
    int   caret_before, caret_after;
    unsigned char group;      /* 1 = continues the previous entry           */
} UcEdit;

typedef struct {
    char  *text;
    int    len, cap;
    char   name[16];          /* 8.3 file name, "" = untitled               */
    char   dir[UC_PATH_MAX];  /* directory, "" = volume root                */
    int    vol;               /* -1 = untitled                              */
    int    lang;
    int    dirty, readonly, exists;
    UcCursor cur[UC_CURSORS_MAX];
    int    ncur;
    int    scroll_line, scroll_col;
    /* line index, rebuilt lazily */
    int   *loff; int nlines, loff_cap; int lines_ok;
    /* tokenizer state at the START of each line, for block comments */
    unsigned short *lstate; int lstate_cap, lstate_lines;
    UcEdit undo[UC_UNDO_MAX];
    int    nundo, undo_at;
    int    saved_at;          /* undo_at at the last save (dirty tracking)  */
    int    grouping, group_fresh;
    unsigned char preview;    /* italic tab title; replaced by the next open */
    /* local history: the text as it was when opened or last saved, so the
     * gutter can show what changed without a version-control system. */
    char  *base; int baselen;
} UcDoc;

int     uc_doc_count(void);
UcDoc  *uc_doc_at(int i);
UcDoc  *uc_doc_active(void);
int     uc_doc_active_index(void);
void    uc_doc_activate(int i);
int     uc_doc_open(int vol, const char *dir, const char *name); /* index    */
int     uc_doc_new(void);
int     uc_doc_close(int i);
int     uc_doc_save(UcDoc *d);
int     uc_doc_save_as(UcDoc *d, int vol, const char *dir, const char *name);
void    uc_doc_free_all(void);
int     uc_doc_title(UcDoc *d, char *out, int cap);
int     uc_doc_path(UcDoc *d, char *out, int cap);

int     uc_line_count(UcDoc *d);
int     uc_line_start(UcDoc *d, int line);
int     uc_line_end(UcDoc *d, int line);
int     uc_line_of(UcDoc *d, int off);
int     uc_col_of(UcDoc *d, int off);
int     uc_offset_of(UcDoc *d, int line, int col);
int     uc_line_state(UcDoc *d, int line);   /* tokenizer state at line start */
int     uc_line_changed(UcDoc *d, int line); /* 0 same, 1 modified, 2 added   */

/* editing - every one of these is undoable and multi-cursor aware */
void    uc_insert(UcDoc *d, const char *s, int n);
void    uc_backspace(UcDoc *d);
void    uc_del_forward(UcDoc *d);
void    uc_newline(UcDoc *d);
void    uc_indent(UcDoc *d, int outdent);
void    uc_undo(UcDoc *d);
void    uc_redo(UcDoc *d);
void    uc_move(UcDoc *d, int dx, int dy, int keep_sel, int by_word);
void    uc_move_to(UcDoc *d, int off, int keep_sel);
void    uc_move_home(UcDoc *d, int keep_sel);
void    uc_move_end(UcDoc *d, int keep_sel);
void    uc_select_all(UcDoc *d);
void    uc_select_word(UcDoc *d);
void    uc_select_line(UcDoc *d);
void    uc_add_cursor(UcDoc *d, int off);
void    uc_add_cursor_line(UcDoc *d, int dir);
void    uc_clear_extra_cursors(UcDoc *d);
int     uc_has_selection(UcDoc *d);
int     uc_selection_text(UcDoc *d, char *out, int cap);
void    uc_delete_selection(UcDoc *d);
void    uc_move_lines(UcDoc *d, int dir);
void    uc_duplicate_lines(UcDoc *d);
void    uc_delete_line(UcDoc *d);
void    uc_toggle_comment(UcDoc *d);
void    uc_begin_group(UcDoc *d);            /* coalesce the next edits      */
void    uc_end_group(UcDoc *d);
int     uc_word_start(UcDoc *d, int off);
int     uc_word_end(UcDoc *d, int off);
int     uc_bracket_match(UcDoc *d, int off);  /* -1 = none                   */
int     uc_indent_of(UcDoc *d, int line, char *pad, int cap);
void    uc_replace_range(UcDoc *d, int a, int b, const char *s, int n);
int     uc_doc_tabsize(UcDoc *d);
int     uc_doc_spaces(UcDoc *d);

/* clipboard (module-wide, shared by every editor and the terminal) */
void        uc_clip_set(const char *s, int n);
const char *uc_clip_get(int *n);

/* ======================================================================== *
 * uc_edit.c - the editor view.
 * ======================================================================== */
typedef struct { int x, y, w, h; } UcRect;

void uc_edit_draw(UcRect r, UcDoc *d, int focused);
int  uc_edit_event(UcRect r, UcDoc *d, const unoui_event *e);
int  uc_edit_key(UcDoc *d, int key, int mods);
int  uc_edit_char(UcDoc *d, int ch);
void uc_edit_reveal(UcRect r, UcDoc *d);
int  uc_edit_rows(UcRect r);
int  uc_char_w(void);
int  uc_line_h(void);
void uc_metrics_init(void);
int  uc_mono_slot(void);
int  uc_font_px(void);
void uc_font_zoom(int delta);
/* draw one monospace string; `style` takes the UC_FS_* bits */
int  uc_mono(int x, int y, const char *s, fb_px fg, int style);
int  uc_mono_n(int x, int y, const char *s, int n, fb_px fg, int style);
/* the proportional UI face used for chrome */
int  uc_ui_text(int x, int y, const char *s, fb_px fg);
int  uc_ui_text_w(const char *s);
int  uc_ui_text_fit(int x, int y, const char *s, int maxw, fb_px fg);
int  uc_ui_h(void);

/* the find widget lives over the editor, so it is part of this view */
void uc_find_open(int replace);
void uc_find_close(void);
int  uc_find_active(void);
int  uc_find_key(int key, int mods, int ch);
void uc_find_draw(UcRect editor_rect);
int  uc_find_event(UcRect editor_rect, const unoui_event *e);
void uc_find_next(int back);
void uc_find_replace(int all);
void uc_find_set(const char *needle);
/* the current search's match runs on `line`, for the editor painter */
int  uc_find_line_hits(UcDoc *d, int line, int *starts, int *ends, int max);

/* IntelliSense */
void uc_suggest_open(UcDoc *d, int explicit_req);
void uc_suggest_close(void);
int  uc_suggest_active(void);
int  uc_suggest_key(int key, int mods);
void uc_suggest_draw(UcRect editor_rect, UcDoc *d);
void uc_suggest_retrigger(UcDoc *d);
/* extensions add items while a request is open (uc_api.c calls this) */
int  uc_suggest_add(const char *label, const char *detail, const char *insert,
                    int kind);
enum { UC_CI_TEXT = 0, UC_CI_METHOD, UC_CI_FUNCTION, UC_CI_VARIABLE,
       UC_CI_CLASS, UC_CI_KEYWORD, UC_CI_SNIPPET, UC_CI_FILE, UC_CI_PROPERTY };

/* ======================================================================== *
 * uc_view.c - the workbench chrome outside the editor.
 * ======================================================================== */
enum { UC_VIEW_EXPLORER = 0, UC_VIEW_SEARCH, UC_VIEW_SCM, UC_VIEW_RUN,
       UC_VIEW_EXTENSIONS, UC_VIEW_N };
enum { UC_PANEL_PROBLEMS = 0, UC_PANEL_OUTPUT, UC_PANEL_TERMINAL,
       UC_PANEL_N };

void uc_view_init(void);
void uc_activity_draw(UcRect r);
int  uc_activity_hit(UcRect r, int x, int y);       /* view index or -1     */
void uc_sidebar_draw(UcRect r);
int  uc_sidebar_event(UcRect r, const unoui_event *e);
int  uc_sidebar_key(int key, int mods, int ch);
void uc_tabs_draw(UcRect r);
int  uc_tabs_event(UcRect r, const unoui_event *e);
void uc_breadcrumb_draw(UcRect r);
void uc_status_draw(UcRect r);
int  uc_status_event(UcRect r, const unoui_event *e);
void uc_panel_draw(UcRect r);
int  uc_panel_event(UcRect r, const unoui_event *e);
int  uc_panel_key(int key, int mods, int ch);
void uc_explorer_refresh(void);
void uc_explorer_reveal(UcDoc *d);
void uc_search_run(const char *needle);
void uc_notif_draw(UcRect r);
void uc_notif_tick(void);

/* problems (diagnostics), populated by builds and by extensions */
enum { UC_SEV_ERROR = 0, UC_SEV_WARN, UC_SEV_INFO };
typedef struct {
    char file[16]; char msg[100]; char source[16];
    int  line, col; unsigned char sev; int vol;
} UcProblem;
void uc_problems_clear(const char *source);
int  uc_problems_add(const UcProblem *p);
int  uc_problems_count(int sev);   /* sev < 0 = all */
UcProblem *uc_problem_at(int i);
int  uc_problems_total(void);

/* output channels - the Output panel's dropdown */
int  uc_output_channel(const char *name);        /* find or create          */
void uc_output_write(int ch, const char *s);
void uc_output_show(int ch);

/* transient notifications (bottom-right toasts) */
void uc_notify(const char *msg, int sev);

/* ======================================================================== *
 * uc_cmd.c - commands, keybindings, the palette and quick open.
 * ======================================================================== */
#define UC_CMD_MAX  220
#define UC_KEYS_MAX 240

typedef void (*UcCmdFn)(void);

typedef struct {
    char id[44];
    char title[52];
    char cat[20];
    UcCmdFn fn;               /* built-in                                   */
    int  ext;                 /* extension index, -1 = built-in             */
    int  jsid;                /* handler slot in uc_api.c                   */
} UcCommand;

typedef struct {
    int  key, mods;           /* first chord                                */
    int  key2, mods2;         /* second chord, key2 = 0 for a single stroke */
    char cmd[44];
    char when[64];
    unsigned char source;     /* 0 default, 1 user, 2 extension             */
    unsigned char removed;    /* a user "-command" entry                    */
} UcKeybind;

void  uc_cmd_init(void);
int   uc_cmd_register(const char *id, const char *title, const char *cat,
                      UcCmdFn fn, int ext, int jsid);
int   uc_cmd_find(const char *id);
int   uc_cmd_run(const char *id);
int   uc_cmd_count(void);
UcCommand *uc_cmd_at(int i);
void  uc_cmd_drop_ext(int ext);            /* on disable/reload             */

int   uc_keys_load(void);                  /* user keybindings.json         */
int   uc_keys_write_reference(void);       /* the defaults, as a file       */
int   uc_keybind_add(int key, int mods, int key2, int mods2,
                     const char *cmd, const char *when, int source);
/* Resolve a key stroke to a command and run it.  Handles chords: returns 2
 * when the stroke opened a chord and the next stroke should come here too. */
int   uc_keys_dispatch(int key, int mods);
const char *uc_keys_label_for(const char *cmd, char *buf, int cap);
int   uc_keys_count(void);
UcKeybind *uc_keybind_at(int i);
int   uc_key_parse(const char *s, int *key, int *mods, int *key2, int *mods2);
int   uc_key_format(int key, int mods, char *out, int cap);
int   uc_chord_pending(void);

/* context keys, for `when` clauses */
void  uc_ctx_set(const char *key, int on);
void  uc_ctx_set_str(const char *key, const char *val);
int   uc_when(const char *expr);           /* empty expr = true             */
void  uc_ctx_refresh(void);

/* the overlay palettes */
enum { UC_Q_NONE = 0, UC_Q_COMMAND, UC_Q_FILE, UC_Q_LINE, UC_Q_SYMBOL,
       UC_Q_THEME, UC_Q_LANG, UC_Q_INPUT, UC_Q_PICK, UC_Q_KEYS };
void uc_quick_open(int mode);
void uc_quick_close(void);
int  uc_quick_active(void);
int  uc_quick_key(int key, int mods, int ch);
void uc_quick_draw(UcRect workbench);
int  uc_quick_event(UcRect workbench, const unoui_event *e);
/* a quick pick / input box driven by an extension (uc_api.c) */
void uc_quick_pick(char (*items)[64], int n, const char *placeholder, int jscb);
void uc_quick_input(const char *placeholder, const char *value, int jscb);

/* fuzzy score used by every list that filters: >= 0 = a match, higher is
 * better; *pos (nullable) receives the matched character indices so the
 * palette can highlight them the way VS Code does. */
int  uc_fuzzy(const char *needle, const char *hay, int *pos, int maxpos);

/* ======================================================================== *
 * uc_ext.c / uc_api.c - the extension host.
 * ======================================================================== */
#define UC_EXT_MAX 24

typedef struct {
    char id[16];              /* the EXT\<ID> directory name                */
    char name[40];            /* displayName                                */
    char publisher[24];
    char version[16];
    char desc[96];
    char main[16];            /* "MAIN.JS", "" = declarative only           */
    int  vol;
    int  enabled, activated, broken;
    int  ncmd, ntheme, ngram, nsnip;
    char err[80];
    unsigned long act_ms;     /* activation cost, shown in the ext view     */
} UcExt;

void  uc_ext_init(void);            /* scan, read manifests, contribute      */
int   uc_ext_count(void);
UcExt *uc_ext_at(int i);
int   uc_ext_enable(int i, int on);
void  uc_ext_activate_event(const char *event);   /* "onCommand:x"           */
void  uc_ext_activate_startup(void);
int   uc_ext_dir(int i, char *out, int cap);      /* "EXT\HELLO"             */
void  uc_ext_reload(void);
int   uc_ext_find(const char *id);
/* snippet completions contributed by extensions, for the suggestion list */
int   uc_ext_snippets(UcDoc *d, const char *prefix);
/* an extension declared this command but its JS has not registered a handler
 * yet: fire its activation events and say whether one appeared. */
int   uc_ext_activate_for_command(const char *id);

/* the JS side */
int   uc_api_init(void);
void  uc_api_shutdown(void);
int   uc_api_run_file(int ext, int vol, const char *path, char *err, int cap);
int   uc_api_call_cmd(int jsid);
int   uc_api_call_str(int jsid, const char *arg);
int   uc_api_call_num(int jsid, int arg);
void  uc_api_pump(void);            /* drain deferred callbacks each frame   */
int   uc_api_alive(void);
const char *uc_api_engine(void);
unsigned long uc_api_fuel(void);
void  uc_api_drop_ext(int ext);
/* the terminal's `js` verb: evaluate one expression and print the result */
void  uc_api_eval_print(const char *expr);
/* language-feature providers registered by extensions */
int   uc_api_completions(UcDoc *d, int offset);   /* items added, or 0       */
int   uc_api_hover(UcDoc *d, int offset, char *out, int cap);
void  uc_api_fire_save(UcDoc *d);
void  uc_api_fire_open(UcDoc *d);
void  uc_api_fire_change(UcDoc *d);

/* ======================================================================== *
 * uc_term.c - the integrated terminal and the task runner.
 * ======================================================================== */
void uc_term_init(void);
void uc_term_draw(UcRect r, int focused);
int  uc_term_key(int key, int mods, int ch);
void uc_term_write(const char *s);
void uc_term_writeln(const char *s);
void uc_term_run(const char *cmdline);
void uc_term_clear(void);
void uc_tasks_run(const char *label);      /* tasks.json                    */
int  uc_tasks_count(void);
const char *uc_task_label(int i);
void uc_tasks_reload(void);
/* Run & Debug: launch.json configurations */
int  uc_launch_count(void);
const char *uc_launch_name(int i);
void uc_launch_run(int i);

/* ======================================================================== *
 * uc_main.c - shared workbench state the other files read.
 * ======================================================================== */
typedef struct {
    UcRect canvas;            /* the whole workbench                        */
    UcRect activity, sidebar, tabs, crumbs, editor, panel, status;
    int    sidebar_w, panel_h;
    int    sidebar_user;      /* the user dragged the splitter: stop sizing it */
    int    sidebar_visible, panel_visible, minimap;
    int    view;              /* UC_VIEW_*                                  */
    int    panel_tab;         /* UC_PANEL_*                                 */
    int    focus;             /* UC_F_*                                     */
    int    zen;
    int    mouse_x, mouse_y, mouse_down, drag;
    int    ws_vol; char ws_dir[UC_PATH_MAX];   /* the open folder           */
    unsigned long frame;
} UcWorkbench;

enum { UC_F_EDITOR = 0, UC_F_SIDEBAR, UC_F_PANEL };
enum { UC_DRAG_NONE = 0, UC_DRAG_SIDEBAR, UC_DRAG_PANEL, UC_DRAG_TEXT,
       UC_DRAG_MINIMAP, UC_DRAG_VSCROLL };

extern UcWorkbench UC;

void uc_layout(void);
void uc_repaint(void);
void uc_focus(int what);
void uc_open_folder(int vol, const char *dir);
int  uc_path_join(char *out, int cap, const char *dir, const char *name);
int  uc_read_file(int vol, const char *path, char **out, long *len);
/* List a directory INCLUDING its subdirectories.  See the implementation in
 * uc_main.c for why this exists rather than uno_fs_list_dir() being used
 * directly - it is the difference between a file tree and a file list. */
int  uc_list_dir(int vol, const char *dir, char (*names)[16],
                 unsigned char *isdir, int maxn);
void uc_status_msg(const char *s);
const char *uc_status_msg_get(void);
void uc_toggle_panel(int tab);
void uc_toggle_sidebar(int view);

/* ---- host queries (uc_main.c, uc_edit.c) -----------------------------------
 * A HOSTED platform - UnoCode Desktop, which owns a real OS window rather than
 * a pc64 canvas - has to answer three questions the pc64 shell never asked:
 * what goes in the title bar, whether closing the window would lose work, and
 * what shape the mouse pointer should be over the thing under it.  None can be
 * answered from outside the subsystem, because UcDoc and UcWorkbench are
 * internal to it, so they are answered here.  Nothing in pc64 calls these; they
 * exist so a host does not have to guess at, or duplicate, the layout. */
enum { UC_CUR_ARROW = 0, UC_CUR_TEXT, UC_CUR_WE, UC_CUR_NS };

const char *uc_host_title(void);        /* "name - folder"; "" if none open  */
int  uc_host_dirty_count(void);         /* editors with unsaved changes      */
int  uc_host_save_all(void);            /* saves every named dirty editor    */
int  uc_host_cursor_at(int x, int y);   /* UC_CUR_*, in canvas pixels        */
int  uc_host_tab_count(void);
/* The i-th open editor's location, for a host restoring a session.  Returns 0
 * for an untitled editor, which has nowhere to be restored from. */
int  uc_host_tab_info(int i, int *vol, char *dir, int dcap,
                      char *name, int ncap);
int  uc_edit_over_text(UcRect r, int x, int y);   /* the I-beam region */

/* small string helpers every file wants */
void uc_scpy(char *d, const char *s, int cap);
void uc_scat(char *d, const char *s, int cap);
int  uc_ieq(const char *a, const char *b);
int  uc_starts(const char *s, const char *pfx);
int  uc_ends_icase(const char *s, const char *suffix);
void uc_upper(char *s);
int  uc_itoa(char *out, long v);
int  uc_is_word(int c);

/* ---- UTF-8 (uc_util.c) -----------------------------------------------------
 * The document is a byte buffer holding UTF-8; these are how the rest of the
 * editor walks it a character at a time.  uc_u8_get() is strict and never
 * consumes more than it was handed, so its return is always a safe step. */
#define UC_CP_BAD 0xFFFD
int  uc_u8_get(const char *s, int n, int *cp);   /* bytes consumed, 1..4     */
int  uc_u8_len(int cp);                          /* bytes cp encodes to      */
int  uc_u8_put(int cp, char *out);               /* encode; bytes written    */
int  uc_u8_align(const char *s, int i);          /* char start at or before i*/
int  uc_u8_back(const char *s, int i);           /* char start before i      */
int  uc_cp_width(int cp);                        /* grid cells: 0, 1 or 2    */

#endif /* UNOCODE_H */
