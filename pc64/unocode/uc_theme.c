/* ===========================================================================
 * uc_theme.c - colour themes.
 *
 * A UnoCode theme file IS a VS Code colour theme, unmodified:
 *
 *     { "name": "Monokai", "type": "dark",
 *       "colors": { "editor.background": "#272822", ... },
 *       "tokenColors": [ { "scope": "comment",
 *                          "settings": { "foreground": "#75715E",
 *                                        "fontStyle": "italic" } } ] }
 *
 * Two rules make that workable on a machine with no npm and no downloads:
 *
 *   1. AN UNKNOWN COLOUR KEY IS IGNORED.  VS Code has several hundred
 *      workbench colours and no theme sets them all; we paint with the ~90 in
 *      the UC_C_* enum.  A theme written against a newer editor must still
 *      load, so unknown keys are skipped, not rejected.
 *   2. AN UNSET KEY IS DERIVED, not left black.  fill_defaults() computes the
 *      whole workbench from `type` plus whatever the theme did set, so a
 *      three-key theme renders correctly instead of rendering as a void.
 *      This is why a hand-written theme in a text editor is a reasonable
 *      thing to do here.
 *
 * The built-ins go through the SAME apply path as a file - they are tables of
 * key/hex pairs, not preassembled palettes - so the loader is exercised on
 * every boot and cannot rot.
 * ======================================================================== */
#include "unocode.h"

typedef struct { const char *key; const char *hex; } UcKV;
typedef struct { const char *scope; const char *hex; const char *style; } UcTK;

typedef struct {
    const char *name;
    int dark, hc;
    const UcKV *colors;
    const UcTK *tokens;
} UcBuiltin;

/* ---- the workbench colour key table --------------------------------------
 * Index-parallel to the UC_C_* enum.  This IS the list of colours a theme can
 * set; keeping it beside the enum is what lets the Settings UI and the theme
 * loader agree without either owning the other. */
static const char *const kColorKey[UC_C_N] = {
    "foreground", "editorError.foreground", "editorWarning.foreground",
    "editorInfo.foreground", "focusBorder",
    "editor.background", "editor.foreground", "editorLineNumber.foreground",
    "editorLineNumber.activeForeground", "editor.selectionBackground",
    "editor.selectionHighlightBackground", "editor.lineHighlightBackground",
    "editorCursor.foreground", "editorWhitespace.foreground",
    "editorIndentGuide.background", "editorIndentGuide.activeBackground",
    "editorBracketMatch.border", "editor.findMatchBackground",
    "editor.findMatchHighlightBackground", "editorRuler.foreground",
    "sideBar.background", "sideBar.foreground", "sideBarTitle.foreground",
    "sideBarSectionHeader.background", "sideBar.border",
    "activityBar.background", "activityBar.foreground",
    "activityBar.inactiveForeground", "activityBar.border",
    "activityBarBadge.background", "activityBarBadge.foreground",
    "statusBar.background", "statusBar.foreground",
    "statusBar.noFolderBackground", "statusBar.debuggingBackground",
    "statusBarItem.hoverBackground",
    "titleBar.activeBackground", "titleBar.activeForeground",
    "editorGroupHeader.tabsBackground", "tab.activeBackground",
    "tab.activeForeground", "tab.inactiveBackground", "tab.inactiveForeground",
    "tab.border", "tab.activeBorderTop", "tab.activeModifiedBorder",
    "breadcrumb.foreground",
    "panel.background", "panel.border", "panelTitle.activeForeground",
    "panelTitle.inactiveForeground",
    "terminal.background", "terminal.foreground", "terminal.ansiRed",
    "terminal.ansiGreen", "terminal.ansiYellow", "terminal.ansiBlue",
    "terminal.ansiCyan",
    "list.activeSelectionBackground", "list.activeSelectionForeground",
    "list.hoverBackground", "list.inactiveSelectionBackground",
    "list.highlightForeground",
    "input.background", "input.foreground", "input.border",
    "input.placeholderForeground",
    "editorWidget.background", "editorWidget.border", "widget.shadow",
    "quickInput.background", "quickInputList.focusBackground",
    "editorSuggestWidget.background", "editorSuggestWidget.selectedBackground",
    "editorSuggestWidget.border",
    "scrollbarSlider.background", "scrollbarSlider.hoverBackground",
    "button.background", "button.foreground", "button.hoverBackground",
    "minimapSlider.background",
    "editorGutter.addedBackground", "editorGutter.modifiedBackground",
    "editorGutter.deletedBackground",
    "gitDecoration.modifiedResourceForeground",
    "gitDecoration.addedResourceForeground",
    "gitDecoration.deletedResourceForeground",
    "notifications.background", "notifications.foreground",
    "notificationCenterHeader.background"
};

const char *uc_color_key(int idx)
{
    return (idx >= 0 && idx < UC_C_N) ? kColorKey[idx] : "";
}

/* ---- built-in themes ------------------------------------------------------
 * Only the colours that DEFINE the theme are listed; everything else is
 * derived.  That keeps each one readable, and it is the same shape a user's
 * hand-written theme file will have. */

static const UcKV kDarkPlus[] = {
    { "editor.background", "#1E1E1E" }, { "editor.foreground", "#D4D4D4" },
    { "editorLineNumber.foreground", "#858585" },
    { "editorLineNumber.activeForeground", "#C6C6C6" },
    { "editor.selectionBackground", "#264F78" },
    { "editor.lineHighlightBackground", "#2A2D2E" },
    { "editorCursor.foreground", "#AEAFAD" },
    { "editorIndentGuide.background", "#404040" },
    { "editor.findMatchBackground", "#515C6A" },
    { "sideBar.background", "#252526" }, { "sideBar.foreground", "#CCCCCC" },
    { "activityBar.background", "#333333" },
    { "activityBar.foreground", "#FFFFFF" },
    { "activityBarBadge.background", "#007ACC" },
    { "statusBar.background", "#007ACC" }, { "statusBar.foreground", "#FFFFFF" },
    { "statusBar.noFolderBackground", "#68217A" },
    { "titleBar.activeBackground", "#3C3C3C" },
    { "editorGroupHeader.tabsBackground", "#252526" },
    { "tab.activeBackground", "#1E1E1E" }, { "tab.activeForeground", "#FFFFFF" },
    { "tab.inactiveBackground", "#2D2D2D" },
    { "tab.inactiveForeground", "#8F8F8F" },
    { "tab.activeBorderTop", "#007ACC" },
    { "panel.background", "#1E1E1E" }, { "panel.border", "#3C3C3C" },
    { "terminal.background", "#1E1E1E" }, { "terminal.foreground", "#CCCCCC" },
    { "list.activeSelectionBackground", "#094771" },
    { "list.hoverBackground", "#2A2D2E" },
    { "list.highlightForeground", "#18A3FF" },
    { "input.background", "#3C3C3C" },
    { "editorWidget.background", "#252526" },
    { "quickInput.background", "#252526" },
    { "button.background", "#0E639C" },
    { "focusBorder", "#007FD4" },
    { 0, 0 }
};
static const UcTK kDarkPlusTok[] = {
    { "comment", "#6A9955", "italic" },
    { "string", "#CE9178", 0 },
    { "constant.numeric", "#B5CEA8", 0 },
    { "constant.language", "#569CD6", 0 },
    { "constant.character", "#D7BA7D", 0 },
    { "keyword", "#C586C0", 0 },
    { "keyword.operator", "#D4D4D4", 0 },
    { "storage", "#569CD6", 0 },
    { "storage.type", "#569CD6", 0 },
    { "entity.name.function", "#DCDCAA", 0 },
    { "entity.name.type", "#4EC9B0", 0 },
    { "entity.name.tag", "#569CD6", 0 },
    { "entity.other.attribute-name", "#9CDCFE", 0 },
    { "support.function", "#DCDCAA", 0 },
    { "support.type", "#4EC9B0", 0 },
    { "support.class", "#4EC9B0", 0 },
    { "variable", "#9CDCFE", 0 },
    { "variable.parameter", "#9CDCFE", 0 },
    { "meta.preprocessor", "#C586C0", 0 },
    { "punctuation.definition.tag", "#808080", 0 },
    { "invalid", "#F44747", 0 },
    { "markup.heading", "#569CD6", "bold" },
    { "markup.bold", "#569CD6", "bold" },
    { "markup.italic", "#D4D4D4", "italic" },
    { "markup.inline.raw", "#CE9178", 0 },
    { "markup.underline.link", "#3794FF", "underline" },
    { 0, 0, 0 }
};

static const UcKV kLightPlus[] = {
    { "editor.background", "#FFFFFF" }, { "editor.foreground", "#000000" },
    { "editorLineNumber.foreground", "#237893" },
    { "editorLineNumber.activeForeground", "#0B216F" },
    { "editor.selectionBackground", "#ADD6FF" },
    { "editor.lineHighlightBackground", "#F0F0F0" },
    { "editorCursor.foreground", "#000000" },
    { "editorIndentGuide.background", "#D3D3D3" },
    { "editor.findMatchBackground", "#A8AC94" },
    { "sideBar.background", "#F3F3F3" }, { "sideBar.foreground", "#333333" },
    { "activityBar.background", "#2C2C2C" },
    { "activityBar.foreground", "#FFFFFF" },
    { "activityBarBadge.background", "#007ACC" },
    { "statusBar.background", "#007ACC" }, { "statusBar.foreground", "#FFFFFF" },
    { "titleBar.activeBackground", "#DDDDDD" },
    { "titleBar.activeForeground", "#333333" },
    { "editorGroupHeader.tabsBackground", "#F3F3F3" },
    { "tab.activeBackground", "#FFFFFF" }, { "tab.activeForeground", "#333333" },
    { "tab.inactiveBackground", "#ECECEC" },
    { "tab.inactiveForeground", "#7A7A7A" },
    { "tab.activeBorderTop", "#007ACC" },
    { "panel.background", "#FFFFFF" }, { "panel.border", "#DDDDDD" },
    { "terminal.background", "#FFFFFF" }, { "terminal.foreground", "#333333" },
    { "list.activeSelectionBackground", "#0060C0" },
    { "list.activeSelectionForeground", "#FFFFFF" },
    { "list.hoverBackground", "#E8E8E8" },
    { "input.background", "#FFFFFF" },
    { "editorWidget.background", "#F3F3F3" },
    { "quickInput.background", "#F3F3F3" },
    { "button.background", "#007ACC" },
    { "focusBorder", "#0090F1" },
    { 0, 0 }
};
static const UcTK kLightPlusTok[] = {
    { "comment", "#008000", 0 },
    { "string", "#A31515", 0 },
    { "constant.numeric", "#098658", 0 },
    { "constant.language", "#0000FF", 0 },
    { "keyword", "#0000FF", 0 },
    { "keyword.operator", "#000000", 0 },
    { "storage", "#0000FF", 0 },
    { "storage.type", "#0000FF", 0 },
    { "entity.name.function", "#795E26", 0 },
    { "entity.name.type", "#267F99", 0 },
    { "entity.name.tag", "#800000", 0 },
    { "entity.other.attribute-name", "#E50000", 0 },
    { "support.function", "#795E26", 0 },
    { "support.type", "#267F99", 0 },
    { "variable", "#001080", 0 },
    { "meta.preprocessor", "#0000FF", 0 },
    { "invalid", "#CD3131", 0 },
    { "markup.heading", "#800000", "bold" },
    { "markup.bold", "#000080", "bold" },
    { "markup.italic", "#000000", "italic" },
    { "markup.inline.raw", "#A31515", 0 },
    { 0, 0, 0 }
};

static const UcKV kMonokai[] = {
    { "editor.background", "#272822" }, { "editor.foreground", "#F8F8F2" },
    { "editorLineNumber.foreground", "#90908A" },
    { "editorLineNumber.activeForeground", "#C2C2BF" },
    { "editor.selectionBackground", "#49483E" },
    { "editor.lineHighlightBackground", "#3E3D32" },
    { "editorCursor.foreground", "#F8F8F0" },
    { "editorIndentGuide.background", "#464741" },
    { "sideBar.background", "#1E1F1C" }, { "sideBar.foreground", "#CFCFC2" },
    { "activityBar.background", "#272822" },
    { "activityBar.foreground", "#F8F8F2" },
    { "activityBarBadge.background", "#75715E" },
    { "statusBar.background", "#414339" }, { "statusBar.foreground", "#F8F8F2" },
    { "titleBar.activeBackground", "#1E1F1C" },
    { "editorGroupHeader.tabsBackground", "#1E1F1C" },
    { "tab.activeBackground", "#272822" }, { "tab.activeForeground", "#F8F8F2" },
    { "tab.inactiveBackground", "#34352F" },
    { "tab.inactiveForeground", "#A59F85" },
    { "tab.activeBorderTop", "#A6E22E" },
    { "panel.background", "#272822" }, { "panel.border", "#414339" },
    { "terminal.background", "#272822" }, { "terminal.foreground", "#F8F8F2" },
    { "list.activeSelectionBackground", "#49483E" },
    { "list.hoverBackground", "#3E3D32" },
    { "input.background", "#414339" },
    { "editorWidget.background", "#1E1F1C" },
    { "quickInput.background", "#1E1F1C" },
    { "button.background", "#75715E" },
    { "focusBorder", "#A6E22E" },
    { 0, 0 }
};
static const UcTK kMonokaiTok[] = {
    { "comment", "#75715E", "italic" },
    { "string", "#E6DB74", 0 },
    { "constant.numeric", "#AE81FF", 0 },
    { "constant.language", "#AE81FF", 0 },
    { "keyword", "#F92672", 0 },
    { "keyword.operator", "#F92672", 0 },
    { "storage", "#F92672", 0 },
    { "storage.type", "#66D9EF", "italic" },
    { "entity.name.function", "#A6E22E", 0 },
    { "entity.name.type", "#A6E22E", "underline" },
    { "entity.name.tag", "#F92672", 0 },
    { "entity.other.attribute-name", "#A6E22E", 0 },
    { "support.function", "#66D9EF", 0 },
    { "support.type", "#66D9EF", "italic" },
    { "variable", "#F8F8F2", 0 },
    { "variable.parameter", "#FD971F", "italic" },
    { "meta.preprocessor", "#F92672", 0 },
    { "invalid", "#F8F8F0", 0 },
    { "markup.heading", "#A6E22E", "bold" },
    { "markup.bold", "#F92672", "bold" },
    { "markup.italic", "#E6DB74", "italic" },
    { "markup.inline.raw", "#E6DB74", 0 },
    { 0, 0, 0 }
};

static const UcKV kSolarized[] = {
    { "editor.background", "#002B36" }, { "editor.foreground", "#839496" },
    { "editorLineNumber.foreground", "#586E75" },
    { "editorLineNumber.activeForeground", "#93A1A1" },
    { "editor.selectionBackground", "#073642" },
    { "editor.lineHighlightBackground", "#073642" },
    { "editorCursor.foreground", "#819090" },
    { "editorIndentGuide.background", "#0E4552" },
    { "sideBar.background", "#00212B" }, { "sideBar.foreground", "#93A1A1" },
    { "activityBar.background", "#003847" },
    { "activityBar.foreground", "#93A1A1" },
    { "activityBarBadge.background", "#2AA198" },
    { "statusBar.background", "#00212B" }, { "statusBar.foreground", "#93A1A1" },
    { "titleBar.activeBackground", "#002B36" },
    { "editorGroupHeader.tabsBackground", "#00212B" },
    { "tab.activeBackground", "#002B36" }, { "tab.activeForeground", "#93A1A1" },
    { "tab.inactiveBackground", "#00212B" },
    { "tab.inactiveForeground", "#586E75" },
    { "tab.activeBorderTop", "#2AA198" },
    { "panel.background", "#002B36" }, { "panel.border", "#0E4552" },
    { "terminal.background", "#002B36" }, { "terminal.foreground", "#839496" },
    { "list.activeSelectionBackground", "#073642" },
    { "list.hoverBackground", "#004052" },
    { "input.background", "#003847" },
    { "editorWidget.background", "#00212B" },
    { "quickInput.background", "#00212B" },
    { "button.background", "#2AA198" },
    { "focusBorder", "#2AA198" },
    { 0, 0 }
};
static const UcTK kSolarizedTok[] = {
    { "comment", "#586E75", "italic" },
    { "string", "#2AA198", 0 },
    { "constant.numeric", "#D33682", 0 },
    { "constant.language", "#B58900", 0 },
    { "keyword", "#859900", 0 },
    { "keyword.operator", "#859900", 0 },
    { "storage", "#268BD2", 0 },
    { "storage.type", "#268BD2", 0 },
    { "entity.name.function", "#268BD2", 0 },
    { "entity.name.type", "#B58900", 0 },
    { "entity.name.tag", "#268BD2", 0 },
    { "support.function", "#268BD2", 0 },
    { "support.type", "#B58900", 0 },
    { "variable", "#839496", 0 },
    { "meta.preprocessor", "#CB4B16", 0 },
    { "invalid", "#DC322F", 0 },
    { "markup.heading", "#268BD2", "bold" },
    { "markup.bold", "#CB4B16", "bold" },
    { "markup.italic", "#839496", "italic" },
    { "markup.inline.raw", "#2AA198", 0 },
    { 0, 0, 0 }
};

static const UcKV kHighContrast[] = {
    { "editor.background", "#000000" }, { "editor.foreground", "#FFFFFF" },
    { "editorLineNumber.foreground", "#FFFFFF" },
    { "editor.selectionBackground", "#FFFFFF" },
    { "editor.lineHighlightBackground", "#000000" },
    { "editorCursor.foreground", "#FFFFFF" },
    { "editorIndentGuide.background", "#FFFFFF" },
    { "sideBar.background", "#000000" }, { "sideBar.foreground", "#FFFFFF" },
    { "activityBar.background", "#000000" },
    { "activityBar.foreground", "#FFFFFF" },
    { "activityBarBadge.background", "#000000" },
    { "statusBar.background", "#000000" }, { "statusBar.foreground", "#FFFFFF" },
    { "titleBar.activeBackground", "#000000" },
    { "editorGroupHeader.tabsBackground", "#000000" },
    { "tab.activeBackground", "#000000" }, { "tab.activeForeground", "#FFFFFF" },
    { "tab.inactiveBackground", "#000000" },
    { "tab.inactiveForeground", "#FFFFFF" },
    { "tab.border", "#6FC3DF" }, { "tab.activeBorderTop", "#6FC3DF" },
    { "panel.background", "#000000" }, { "panel.border", "#6FC3DF" },
    { "terminal.background", "#000000" }, { "terminal.foreground", "#FFFFFF" },
    { "list.activeSelectionBackground", "#000000" },
    { "input.background", "#000000" }, { "input.border", "#6FC3DF" },
    { "editorWidget.background", "#0C141F" },
    { "editorWidget.border", "#6FC3DF" },
    { "quickInput.background", "#0C141F" },
    { "button.background", "#0F4A85" },
    { "focusBorder", "#F38518" },
    { 0, 0 }
};
static const UcTK kHighContrastTok[] = {
    { "comment", "#7CA668", "italic" },
    { "string", "#CE9178", 0 },
    { "constant.numeric", "#B5CEA8", 0 },
    { "keyword", "#569CD6", 0 },
    { "storage.type", "#569CD6", 0 },
    { "entity.name.function", "#DCDCAA", 0 },
    { "entity.name.type", "#4EC9B0", 0 },
    { "variable", "#9CDCFE", 0 },
    { "invalid", "#F48771", 0 },
    { 0, 0, 0 }
};

/* The house theme: UnoDOS's own blue desktop, so UnoCode looks like part of
 * the system it runs on rather than a visitor from another one. */
static const UcKV kUnoBlue[] = {
    { "editor.background", "#04123A" }, { "editor.foreground", "#DCE6FF" },
    { "editorLineNumber.foreground", "#4B6CB0" },
    { "editorLineNumber.activeForeground", "#8FB4FF" },
    { "editor.selectionBackground", "#1B3B84" },
    { "editor.lineHighlightBackground", "#0A1C50" },
    { "editorCursor.foreground", "#00E5FF" },
    { "editorIndentGuide.background", "#1C2E60" },
    { "sideBar.background", "#020C28" }, { "sideBar.foreground", "#BFD0F5" },
    { "activityBar.background", "#010819" },
    { "activityBar.foreground", "#00E5FF" },
    { "activityBarBadge.background", "#00AACC" },
    { "statusBar.background", "#00AACC" }, { "statusBar.foreground", "#00121A" },
    { "titleBar.activeBackground", "#020C28" },
    { "editorGroupHeader.tabsBackground", "#020C28" },
    { "tab.activeBackground", "#04123A" }, { "tab.activeForeground", "#FFFFFF" },
    { "tab.inactiveBackground", "#03102F" },
    { "tab.inactiveForeground", "#7E93C8" },
    { "tab.activeBorderTop", "#00E5FF" },
    { "panel.background", "#04123A" }, { "panel.border", "#1C2E60" },
    { "terminal.background", "#010819" }, { "terminal.foreground", "#CFE3FF" },
    { "list.activeSelectionBackground", "#12306E" },
    { "list.hoverBackground", "#0A1C50" },
    { "input.background", "#0A1C50" },
    { "editorWidget.background", "#020C28" },
    { "quickInput.background", "#020C28" },
    { "button.background", "#00AACC" },
    { "focusBorder", "#00E5FF" },
    { 0, 0 }
};
static const UcTK kUnoBlueTok[] = {
    { "comment", "#5A79B8", "italic" },
    { "string", "#7FE8C0", 0 },
    { "constant.numeric", "#F7C873", 0 },
    { "constant.language", "#FF8AD8", 0 },
    { "keyword", "#FF8AD8", 0 },
    { "keyword.operator", "#DCE6FF", 0 },
    { "storage.type", "#00E5FF", 0 },
    { "entity.name.function", "#FFE58F", 0 },
    { "entity.name.type", "#00E5FF", 0 },
    { "variable", "#BFD0F5", 0 },
    { "meta.preprocessor", "#FF8AD8", 0 },
    { "invalid", "#FF5C7A", 0 },
    { "markup.heading", "#00E5FF", "bold" },
    { 0, 0, 0 }
};

static const UcBuiltin kBuiltin[] = {
    { "Dark+ (default dark)",   1, 0, kDarkPlus,      kDarkPlusTok },
    { "Light+ (default light)", 0, 0, kLightPlus,     kLightPlusTok },
    { "Monokai",                1, 0, kMonokai,       kMonokaiTok },
    { "Solarized Dark",         1, 0, kSolarized,     kSolarizedTok },
    { "High Contrast",          1, 1, kHighContrast,  kHighContrastTok },
    { "UnoDOS Blue",            1, 0, kUnoBlue,       kUnoBlueTok }
};
#define NBUILTIN ((int)(sizeof kBuiltin / sizeof kBuiltin[0]))

/* ---- state ---------------------------------------------------------------- */
static UcTheme g_theme[UC_THEMES_MAX];
static int     g_ntheme;
static int     g_active;
static unsigned char g_set[UC_THEMES_MAX][UC_C_N];   /* explicitly assigned */

int      uc_theme_count(void) { return g_ntheme; }
UcTheme *uc_theme_at(int i)   { return (i >= 0 && i < g_ntheme) ? &g_theme[i] : 0; }
UcTheme *uc_theme_active(void){ return &g_theme[g_active]; }

/* ---- colour arithmetic ---------------------------------------------------- */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int uc_parse_hex_color(const char *s, fb_px *out, int *alpha)
{
    int v[8], n = 0, r, g, b, a = 255;
    if (!s) return 0;
    if (*s == '#') s++;
    while (s[n] && n < 8) {
        int h = hexval((unsigned char)s[n]);
        if (h < 0) break;
        v[n] = h;
        n++;
    }
    if (n == 3 || n == 4) {
        r = v[0] * 17; g = v[1] * 17; b = v[2] * 17;
        if (n == 4) a = v[3] * 17;
    } else if (n == 6 || n == 8) {
        r = v[0] * 16 + v[1]; g = v[2] * 16 + v[3]; b = v[4] * 16 + v[5];
        if (n == 8) a = v[6] * 16 + v[7];
    } else return 0;
    if (out) *out = FB_RGB(r, g, b);
    if (alpha) *alpha = a;
    return 1;
}

fb_px uc_blend(fb_px a, fb_px b, int alpha)
{
    int ar = a & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    int br = b & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    return FB_RGB(br + (ar - br) * alpha / 255,
                  bg + (ag - bg) * alpha / 255,
                  bb + (ab - bb) * alpha / 255);
}

/* mix towards white (amount > 0) or black (amount < 0), -255..255 */
static fb_px shade(fb_px c, int amount)
{
    return uc_blend(amount >= 0 ? FB_RGB(255,255,255) : FB_RGB(0,0,0), c,
                    amount >= 0 ? amount : -amount);
}

static int key_index(const char *key)
{
    int i;
    for (i = 0; i < UC_C_N; i++)
        if (!strcmp(kColorKey[i], key)) return i;
    return -1;
}

/* Set one workbench colour.  A colour given with alpha is flattened against
 * the editor background NOW, because there is no alpha buffer to composite it
 * into later and "#264F7880" must not read as opaque #264F78. */
static void set_color(int ti, const char *key, const char *hex)
{
    int idx = key_index(key), alpha = 255;
    fb_px c;
    if (idx < 0) return;                        /* unknown key: ignored      */
    if (!uc_parse_hex_color(hex, &c, &alpha)) return;
    if (alpha < 255) {
        fb_px bg = g_set[ti][UC_C_EDITOR_BG] ? g_theme[ti].c[UC_C_EDITOR_BG]
                                             : (g_theme[ti].dark ? FB_RGB(30,30,30)
                                                                 : FB_RGB(255,255,255));
        c = uc_blend(c, bg, alpha);
    }
    g_theme[ti].c[idx] = c;
    g_set[ti][idx] = 1;
}

static int style_bits(const char *s)
{
    int b = 0;
    if (!s) return 0;
    if (strstr(s, "bold"))      b |= UC_FS_BOLD;
    if (strstr(s, "italic"))    b |= UC_FS_ITALIC;
    if (strstr(s, "underline")) b |= UC_FS_UNDERLINE;
    return b;
}

static void add_token(int ti, const char *scope, const char *hex, int style)
{
    UcTheme *t = &g_theme[ti];
    fb_px c = 0;
    if (t->ntok >= UC_TOK_SCOPES) return;
    uc_scpy(t->tok[t->ntok].scope, scope, sizeof t->tok[0].scope);
    if (hex && uc_parse_hex_color(hex, &c, 0)) {
        t->tok[t->ntok].fg = c;
        t->tok[t->ntok].has_fg = 1;
    }
    t->tok[t->ntok].style = (unsigned char)style;
    t->ntok++;
}

/* ---- default derivation ---------------------------------------------------
 * Everything a theme did not set.  Order matters: the later entries lean on
 * the earlier ones, so a theme that sets only editor.background/foreground
 * still gets a coherent workbench rather than a black void with black text. */
#define DEF(idx, val) do { if (!g_set[ti][idx]) t->c[idx] = (val); } while (0)

static void fill_defaults(int ti)
{
    UcTheme *t = &g_theme[ti];
    fb_px bg, fg, side, acc;
    int d = t->dark;

    DEF(UC_C_EDITOR_BG, d ? FB_RGB(30, 30, 30) : FB_RGB(255, 255, 255));
    DEF(UC_C_EDITOR_FG, d ? FB_RGB(212, 212, 212) : FB_RGB(0, 0, 0));
    bg = t->c[UC_C_EDITOR_BG];
    fg = t->c[UC_C_EDITOR_FG];

    DEF(UC_C_FG, fg);
    DEF(UC_C_FOCUS_BORDER, d ? FB_RGB(0, 127, 212) : FB_RGB(0, 144, 241));
    acc = t->c[UC_C_FOCUS_BORDER];
    DEF(UC_C_ERROR_FG, FB_RGB(244, 71, 71));
    DEF(UC_C_WARN_FG,  FB_RGB(204, 167, 0));
    DEF(UC_C_INFO_FG,  FB_RGB(117, 190, 255));

    DEF(UC_C_LINENO, uc_blend(fg, bg, 90));
    DEF(UC_C_LINENO_ACTIVE, uc_blend(fg, bg, 200));
    DEF(UC_C_SELECTION, d ? FB_RGB(38, 79, 120) : FB_RGB(173, 214, 255));
    DEF(UC_C_SELECTION_HL, uc_blend(t->c[UC_C_SELECTION], bg, 150));
    DEF(UC_C_LINE_HL, shade(bg, d ? 18 : -12));
    DEF(UC_C_CURSOR, d ? FB_RGB(174, 175, 173) : FB_RGB(0, 0, 0));
    DEF(UC_C_WHITESPACE, uc_blend(fg, bg, 60));
    DEF(UC_C_INDENT_GUIDE, uc_blend(fg, bg, 45));
    DEF(UC_C_INDENT_GUIDE_ACTIVE, uc_blend(fg, bg, 110));
    DEF(UC_C_BRACKET_MATCH, uc_blend(fg, bg, 140));
    DEF(UC_C_FIND_MATCH, d ? FB_RGB(81, 92, 106) : FB_RGB(168, 172, 148));
    DEF(UC_C_FIND_MATCH_HL, uc_blend(t->c[UC_C_FIND_MATCH], bg, 150));
    DEF(UC_C_RULER, uc_blend(fg, bg, 40));

    DEF(UC_C_SIDEBAR_BG, shade(bg, d ? 10 : -8));
    side = t->c[UC_C_SIDEBAR_BG];
    DEF(UC_C_SIDEBAR_FG, uc_blend(fg, side, 210));
    DEF(UC_C_SIDEBAR_TITLE, uc_blend(fg, side, 160));
    DEF(UC_C_SIDEBAR_SECT, shade(side, d ? 12 : -10));
    DEF(UC_C_SIDEBAR_BORDER, shade(side, d ? 25 : -25));

    DEF(UC_C_ACTIVITY_BG, shade(bg, d ? 20 : -40));
    DEF(UC_C_ACTIVITY_FG, d ? FB_RGB(255,255,255) : FB_RGB(255,255,255));
    DEF(UC_C_ACTIVITY_DIM, uc_blend(t->c[UC_C_ACTIVITY_FG],
                                    t->c[UC_C_ACTIVITY_BG], 110));
    DEF(UC_C_ACTIVITY_BORDER, shade(t->c[UC_C_ACTIVITY_BG], d ? 25 : 25));
    DEF(UC_C_BADGE_BG, acc);
    DEF(UC_C_BADGE_FG, FB_RGB(255, 255, 255));

    DEF(UC_C_STATUS_BG, acc);
    DEF(UC_C_STATUS_FG, FB_RGB(255, 255, 255));
    DEF(UC_C_STATUS_NOFOLDER, FB_RGB(104, 33, 122));
    DEF(UC_C_STATUS_DEBUG, FB_RGB(204, 102, 51));
    DEF(UC_C_STATUS_HOVER, shade(t->c[UC_C_STATUS_BG], 40));

    DEF(UC_C_TITLE_BG, shade(bg, d ? 22 : -18));
    DEF(UC_C_TITLE_FG, uc_blend(fg, t->c[UC_C_TITLE_BG], 200));

    DEF(UC_C_TABS_BG, side);
    DEF(UC_C_TAB_ACTIVE_BG, bg);
    DEF(UC_C_TAB_ACTIVE_FG, uc_blend(fg, bg, 255));
    DEF(UC_C_TAB_INACTIVE_BG, shade(side, d ? 8 : -6));
    DEF(UC_C_TAB_INACTIVE_FG, uc_blend(fg, side, 130));
    DEF(UC_C_TAB_BORDER, shade(side, d ? 20 : -20));
    DEF(UC_C_TAB_ACTIVE_TOP, acc);
    DEF(UC_C_TAB_MODIFIED, acc);
    DEF(UC_C_BREADCRUMB_FG, uc_blend(fg, bg, 140));

    DEF(UC_C_PANEL_BG, bg);
    DEF(UC_C_PANEL_BORDER, shade(bg, d ? 30 : -25));
    DEF(UC_C_PANEL_TITLE, uc_blend(fg, bg, 255));
    DEF(UC_C_PANEL_TITLE_DIM, uc_blend(fg, bg, 120));

    DEF(UC_C_TERM_BG, bg);
    DEF(UC_C_TERM_FG, fg);
    DEF(UC_C_TERM_RED,    d ? FB_RGB(241,  76,  76) : FB_RGB(205,  49,  49));
    DEF(UC_C_TERM_GREEN,  d ? FB_RGB( 35, 209, 139) : FB_RGB( 14, 102,  85));
    DEF(UC_C_TERM_YELLOW, d ? FB_RGB(245, 245,  67) : FB_RGB(148, 133,   0));
    DEF(UC_C_TERM_BLUE,   d ? FB_RGB( 59, 142, 234) : FB_RGB(  0,   0, 187));
    DEF(UC_C_TERM_CYAN,   d ? FB_RGB( 41, 184, 219) : FB_RGB( 17, 168, 205));

    DEF(UC_C_LIST_SEL_BG, d ? FB_RGB(9, 71, 113) : FB_RGB(0, 96, 192));
    DEF(UC_C_LIST_SEL_FG, FB_RGB(255, 255, 255));
    DEF(UC_C_LIST_HOVER_BG, shade(side, d ? 14 : -12));
    DEF(UC_C_LIST_INACTIVE_BG, uc_blend(t->c[UC_C_LIST_SEL_BG], side, 120));
    DEF(UC_C_LIST_HIGHLIGHT, d ? FB_RGB(24, 163, 255) : FB_RGB(6, 125, 214));

    DEF(UC_C_INPUT_BG, shade(bg, d ? 28 : -6));
    DEF(UC_C_INPUT_FG, fg);
    DEF(UC_C_INPUT_BORDER, shade(t->c[UC_C_INPUT_BG], d ? 30 : -30));
    DEF(UC_C_INPUT_PLACEHOLDER, uc_blend(fg, t->c[UC_C_INPUT_BG], 110));

    DEF(UC_C_WIDGET_BG, side);
    DEF(UC_C_WIDGET_BORDER, shade(side, d ? 35 : -35));
    DEF(UC_C_WIDGET_SHADOW, FB_RGB(0, 0, 0));
    DEF(UC_C_QUICK_BG, t->c[UC_C_WIDGET_BG]);
    DEF(UC_C_QUICK_SEL_BG, t->c[UC_C_LIST_SEL_BG]);
    DEF(UC_C_SUGGEST_BG, t->c[UC_C_WIDGET_BG]);
    DEF(UC_C_SUGGEST_SEL_BG, t->c[UC_C_LIST_SEL_BG]);
    DEF(UC_C_SUGGEST_BORDER, t->c[UC_C_WIDGET_BORDER]);

    DEF(UC_C_SCROLL_SLIDER, uc_blend(fg, bg, 40));
    DEF(UC_C_SCROLL_HOVER, uc_blend(fg, bg, 70));
    DEF(UC_C_MINIMAP_SLIDER, uc_blend(fg, bg, 30));

    DEF(UC_C_BUTTON_BG, acc);
    DEF(UC_C_BUTTON_FG, FB_RGB(255, 255, 255));
    DEF(UC_C_BUTTON_HOVER, shade(t->c[UC_C_BUTTON_BG], 30));

    DEF(UC_C_GUTTER_ADDED,    FB_RGB( 88, 124,  12));
    DEF(UC_C_GUTTER_MODIFIED, FB_RGB( 12,  95, 173));
    DEF(UC_C_GUTTER_DELETED,  FB_RGB(148,  21,  27));
    DEF(UC_C_GIT_ADDED,    FB_RGB(129, 184, 139));
    DEF(UC_C_GIT_MODIFIED, FB_RGB(226, 192, 141));
    DEF(UC_C_GIT_DELETED,  FB_RGB(199, 120, 118));

    DEF(UC_C_NOTIF_BG, t->c[UC_C_WIDGET_BG]);
    DEF(UC_C_NOTIF_FG, fg);
    DEF(UC_C_NOTIF_BORDER, t->c[UC_C_WIDGET_BORDER]);
}
#undef DEF

/* ---- registration --------------------------------------------------------- */
static int theme_slot(const char *label)
{
    int i;
    for (i = 0; i < g_ntheme; i++)
        if (!strcmp(g_theme[i].name, label)) return i;
    if (g_ntheme >= UC_THEMES_MAX) return -1;
    i = g_ntheme++;
    memset(&g_theme[i], 0, sizeof g_theme[i]);
    memset(g_set[i], 0, UC_C_N);
    uc_scpy(g_theme[i].name, label, sizeof g_theme[i].name);
    g_theme[i].ext = -1;
    g_theme[i].vol = -1;
    return i;
}

static void load_builtin(int ti, const UcBuiltin *b)
{
    int k;
    g_theme[ti].dark = (unsigned char)b->dark;
    g_theme[ti].hc   = (unsigned char)b->hc;
    g_theme[ti].builtin = 1;
    g_theme[ti].ntok = 0;
    memset(g_set[ti], 0, UC_C_N);
    for (k = 0; b->colors[k].key; k++) set_color(ti, b->colors[k].key, b->colors[k].hex);
    for (k = 0; b->tokens[k].scope; k++)
        add_token(ti, b->tokens[k].scope, b->tokens[k].hex, style_bits(b->tokens[k].style));
    fill_defaults(ti);
    g_theme[ti].loaded = 1;
}

/* Read a VS Code theme file into slot `ti`. */
static int load_file(int ti)
{
    UcTheme *t = &g_theme[ti];
    char *src = 0;
    long len = 0;
    char err[80];
    UcJson *root, *colors, *toks, *m;
    const char *type;
    if (t->vol < 0 || !t->file[0]) return 0;
    if (!uc_read_file(t->vol, t->file, &src, &len)) return 0;
    root = uc_json_parse(src, (int)len, err, sizeof err);
    free(src);
    if (!root) return 0;

    type = uc_json_str(root, "type", t->dark ? "dark" : "light");
    t->dark = (unsigned char)(strcmp(type, "light") != 0);
    t->hc   = (unsigned char)(uc_starts(type, "hc") ? 1 : 0);
    t->ntok = 0;
    memset(g_set[ti], 0, UC_C_N);

    colors = uc_json_member(root, "colors");
    if (colors) {
        /* editor.background first: a translucent colour is flattened against
         * it, and a theme that lists it late would otherwise flatten the
         * earlier ones against the wrong ground. */
        for (m = colors->child; m; m = m->next)
            if (m->key && m->type == UJ_STR && !strcmp(m->key, "editor.background"))
                set_color(ti, m->key, m->str);
        for (m = colors->child; m; m = m->next)
            if (m->key && m->type == UJ_STR) set_color(ti, m->key, m->str);
    }
    toks = uc_json_member(root, "tokenColors");
    if (toks && toks->type == UJ_ARR) {
        UcJson *e;
        for (e = toks->child; e; e = e->next) {
            UcJson *sc = uc_json_member(e, "scope");
            UcJson *st = uc_json_member(e, "settings");
            const char *fgs = st ? uc_json_str(st, "foreground", 0) : 0;
            int style = style_bits(st ? uc_json_str(st, "fontStyle", 0) : 0);
            if (!st) continue;
            if (!sc) { add_token(ti, "", fgs, style); continue; }
            if (sc->type == UJ_STR) {
                /* "keyword, storage" - a comma list of scopes in one rule */
                const char *p = sc->str;
                while (*p) {
                    char one[52];
                    int n = 0;
                    while (*p == ' ' || *p == ',') p++;
                    while (*p && *p != ',' && n < (int)sizeof one - 1) one[n++] = *p++;
                    while (n > 0 && one[n-1] == ' ') n--;
                    one[n] = 0;
                    if (n) add_token(ti, one, fgs, style);
                }
            } else if (sc->type == UJ_ARR) {
                UcJson *s2;
                for (s2 = sc->child; s2; s2 = s2->next)
                    if (s2->type == UJ_STR) add_token(ti, s2->str, fgs, style);
            }
        }
    }
    uc_json_free(root);
    fill_defaults(ti);
    t->loaded = 1;
    return 1;
}

int uc_theme_register(const char *label, int vol, const char *path, int dark,
                      int ext)
{
    int ti = theme_slot(label);
    if (ti < 0) return -1;
    g_theme[ti].vol = (signed char)vol;
    uc_scpy(g_theme[ti].file, path, sizeof g_theme[ti].file);
    g_theme[ti].dark = (unsigned char)dark;
    g_theme[ti].ext = ext;
    g_theme[ti].builtin = 0;
    g_theme[ti].loaded = 0;
    return ti;
}

int uc_theme_find(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < g_ntheme; i++)
        if (!strcmp(g_theme[i].name, name)) return i;
    /* be forgiving about the parenthesised suffix: "Dark+" finds
     * "Dark+ (default dark)", which is what people actually type */
    for (i = 0; i < g_ntheme; i++)
        if (uc_starts(g_theme[i].name, name)) return i;
    return -1;
}

int uc_theme_select(const char *name)
{
    int i = uc_theme_find(name);
    if (i < 0) return 0;
    if (!g_theme[i].loaded) {
        if (!load_file(i)) {
            /* a broken theme file must not leave the workbench unpainted */
            memset(g_set[i], 0, UC_C_N);
            g_theme[i].ntok = 0;
            fill_defaults(i);
            g_theme[i].loaded = 1;
        }
    }
    if (i == g_active) return 0;
    g_active = i;
    return 1;
}

void uc_theme_init(void)
{
    int i;
    g_ntheme = 0;
    g_active = 0;
    for (i = 0; i < NBUILTIN; i++) {
        int ti = theme_slot(kBuiltin[i].name);
        if (ti >= 0) load_builtin(ti, &kBuiltin[i]);
    }
}

fb_px uc_col(int idx)
{
    if (idx < 0 || idx >= UC_C_N) return FB_RGB(255, 0, 255);
    return g_theme[g_active].c[idx];
}

/* ---- token scope resolution -----------------------------------------------
 * TextMate's rule: the theme rule whose scope is the longest DOTTED PREFIX of
 * the token's scope wins.  "keyword.control.c" is matched by "keyword" and by
 * "keyword.control", and the latter wins; it is NOT matched by "key".  An
 * empty rule scope is the theme's default foreground and matches everything
 * at length zero. */
fb_px uc_tok_color(const char *scope, int *style)
{
    UcTheme *t = &g_theme[g_active];
    int i, best = -1, bestlen = -1;
    if (style) *style = 0;
    if (!scope || !scope[0]) return t->c[UC_C_EDITOR_FG];
    for (i = 0; i < t->ntok; i++) {
        const char *rs = t->tok[i].scope;
        int n = (int)strlen(rs);
        if (n == 0) { if (bestlen < 0) { best = i; bestlen = 0; } continue; }
        if (strncmp(scope, rs, (unsigned long)n)) continue;
        if (scope[n] && scope[n] != '.') continue;      /* dot boundary only */
        if (n > bestlen) { bestlen = n; best = i; }
    }
    if (best < 0) return t->c[UC_C_EDITOR_FG];
    if (style) *style = t->tok[best].style;
    return t->tok[best].has_fg ? t->tok[best].fg : t->c[UC_C_EDITOR_FG];
}
