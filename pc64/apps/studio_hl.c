/* Studio - the UnoC syntax highlighter.  See studio_hl.h. */
#include "studio_hl.h"

static int is_id0(int c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_id (int c){ return is_id0(c)||(c>='0'&&c<='9'); }
static int is_dig(int c){ return c>='0'&&c<='9'; }

/* the UnoC keyword + type sets (kept small; matches docs_esp/LANG.MD) */
static int classify_word(const char *p, int n)
{
    static const char *const kw[] = {
        "if","else","while","do","for","switch","case","default","break",
        "continue","return","sizeof","struct","union","enum","typedef",
        "static","extern","const","goto", 0 };
    static const char *const ty[] = {
        "void","char","short","int","long","unsigned","signed",
        "Boolean","Rect","Point","RGBColor","UnoWin","KernelApi",
        "AppInterface","GameRGB","Note","Song","Ptr", 0 };
    int i, j;
    for (i = 0; kw[i]; i++) {
        for (j = 0; j < n && kw[i][j] && kw[i][j] == p[j]; j++) {}
        if (j == n && !kw[i][j]) return HL_KW;
    }
    for (i = 0; ty[i]; i++) {
        for (j = 0; j < n && ty[i][j] && ty[i][j] == p[j]; j++) {}
        if (j == n && !ty[i][j]) return HL_TYPE;
    }
    return HL_PLAIN;
}

int studio_hl_line(const char *p, int n, int *in_comment, HlSpan *sp, int maxsp)
{
    int i = 0, ns = 0;
    #define EMIT(s, l, c) do { if (ns < maxsp) { sp[ns].start = (short)(s); \
        sp[ns].len = (short)(l); sp[ns].cls = (unsigned char)(c); ns++; } } while (0)

    if (*in_comment) {                      /* continue a block comment */
        int s = 0;
        while (i < n) {
            if (p[i] == '*' && i + 1 < n && p[i+1] == '/') { i += 2; *in_comment = 0; break; }
            i++;
        }
        EMIT(s, i - s, HL_COMMENT);
    }

    while (i < n) {
        int c = (unsigned char)p[i];
        if (c == ' ' || c == '\t') { i++; continue; }

        if (c == '/' && i + 1 < n && p[i+1] == '/') {      /* line comment */
            EMIT(i, n - i, HL_COMMENT); i = n; break;
        }
        if (c == '/' && i + 1 < n && p[i+1] == '*') {      /* block comment */
            int s = i; i += 2;
            *in_comment = 1;
            while (i < n) {
                if (p[i] == '*' && i + 1 < n && p[i+1] == '/') { i += 2; *in_comment = 0; break; }
                i++;
            }
            EMIT(s, i - s, HL_COMMENT);
            continue;
        }
        if (c == '#') {                                     /* preprocessor */
            /* the whole line is a directive (leading ws already skipped) */
            EMIT(i, n - i, HL_PREPROC); i = n; break;
        }
        if (c == '"') {                                     /* string */
            int s = i; i++;
            while (i < n) { if (p[i] == '\\' && i+1 < n) { i += 2; continue; }
                            if (p[i] == '"') { i++; break; } i++; }
            EMIT(s, i - s, HL_STR);
            continue;
        }
        if (c == '\'') {                                    /* char */
            int s = i; i++;
            while (i < n) { if (p[i] == '\\' && i+1 < n) { i += 2; continue; }
                            if (p[i] == '\'') { i++; break; } i++; }
            EMIT(s, i - s, HL_CHAR);
            continue;
        }
        if (is_dig(c) || (c == '.' && i+1 < n && is_dig((unsigned char)p[i+1]))) {
            int s = i;
            if (c == '0' && i+1 < n && (p[i+1]=='x'||p[i+1]=='X')) {
                i += 2; while (i < n && (is_dig((unsigned char)p[i]) ||
                    (p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F'))) i++;
            } else {
                while (i < n && (is_dig((unsigned char)p[i]) || p[i]=='.')) i++;
            }
            while (i < n && (p[i]=='u'||p[i]=='U'||p[i]=='l'||p[i]=='L')) i++;
            EMIT(s, i - s, HL_NUM);
            continue;
        }
        if (is_id0(c)) {                                    /* word */
            int s = i;
            while (i < n && is_id((unsigned char)p[i])) i++;
            EMIT(s, i - s, classify_word(p + s, i - s));
            continue;
        }
        i++;                                                /* punctuation */
    }
    #undef EMIT
    return ns;
}
