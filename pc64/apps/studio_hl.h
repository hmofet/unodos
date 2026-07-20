/* Studio - the UnoC syntax highlighter (studio_hl.c).
 *
 * A per-line tokenizer: hand it a line and the "inside a block comment"
 * state carried in from the previous line, and it fills a span array and
 * returns the state to carry to the next line.  No stored colours - the
 * editor recomputes visible lines each frame, which is trivially fast for
 * an 80x50 view. */
#ifndef STUDIO_HL_H
#define STUDIO_HL_H

enum {
    HL_PLAIN = 0, HL_KW, HL_TYPE, HL_STR, HL_CHAR, HL_NUM,
    HL_COMMENT, HL_PREPROC, HL_PUNCT, HL_NCLASS
};

typedef struct { short start, len; unsigned char cls; } HlSpan;

/* tokenize one line ([p,p+n)); returns the number of spans written (<= maxsp)
 * and updates *in_comment (1 = the next line starts inside a block comment). */
int studio_hl_line(const char *p, int n, int *in_comment,
                   HlSpan *sp, int maxsp);

#endif
