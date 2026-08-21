/* ===========================================================================
 * uno_utf8.h - UTF-8, for everything that has to walk text a CHARACTER at a
 * time rather than a byte at a time.
 *
 * A HEADER of static functions rather than a .c, and that is deliberate: the
 * two callers sit on opposite sides of a link boundary.  pc64_font.c is in the
 * kernel; unocode's uc_util.c ships inside UNOCODE.UNO, a loadable module.  A
 * shared object file would leave the kernel with an undefined symbol whenever
 * the module was not linked in.  The code is small enough that a copy in each
 * translation unit costs less than the coupling would.
 *
 * Decoding is STRICT.  An over-long form, a surrogate, a value past U+10FFFF
 * and a truncated tail all decode as ONE byte carrying U+FFFD.  A lenient
 * decoder that swallowed the whole malformed run would shift every offset
 * after it, and an editor that quietly renumbers a file's bytes does more
 * damage than one that draws a broken byte as broken.  One byte per bad byte
 * also means uno_u8_get() never reports more than it was given, which is what
 * lets every caller treat the return as a safe step.
 * ======================================================================== */
#ifndef UNO_UTF8_H
#define UNO_UTF8_H

#define UNO_CP_BAD 0xFFFD

/* Decode the character at `s`, looking at no more than `n` bytes.  Returns the
 * bytes consumed (1..4), or 0 when n <= 0. */
static inline int uno_u8_get(const char *s, int n, int *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    int c, need, i, v;

    if (n <= 0) { *cp = 0; return 0; }
    c = p[0];
    if (c < 0x80)                { *cp = c;  return 1; }
    else if ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; }
    else                         { *cp = UNO_CP_BAD; return 1; }

    if (n <= need) { *cp = UNO_CP_BAD; return 1; }
    for (i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = UNO_CP_BAD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    /* over-long, surrogate, out of range: every one of them is one bad byte */
    if ((need == 1 && v < 0x80) || (need == 2 && v < 0x800) ||
        (need == 3 && v < 0x10000) || v > 0x10FFFF ||
        (v >= 0xD800 && v <= 0xDFFF)) { *cp = UNO_CP_BAD; return 1; }
    *cp = v;
    return need + 1;
}

static inline int uno_u8_len(int cp)
{
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 3;
    if (cp < 0x80)    return 1;
    if (cp < 0x800)   return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

/* Encode `cp` into `out` (at most 4 bytes, never NUL-terminated).  Returns the
 * bytes written. */
static inline int uno_u8_put(int cp, char *out)
{
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = UNO_CP_BAD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* The offset of the character START at or before `i`.  Bounded to three steps,
 * so a run of stray continuation bytes cannot walk it off the front. */
static inline int uno_u8_align(const char *s, int i)
{
    int k = 0;
    if (i <= 0) return 0;
    while (i > 0 && k < 3 && ((unsigned char)s[i] & 0xC0) == 0x80) { i--; k++; }
    return i;
}

/* Cells this character occupies in a fixed grid: 0 for a combining mark, 2 for
 * the East Asian wide and emoji blocks, 1 for everything else.  Ranges rather
 * than the real Unicode table, which is 40 KB of data that would need
 * maintaining; these are the blocks a source file actually contains.  Getting
 * a 2 wrong is not cosmetic - the caret, the selection and the mouse hit test
 * all multiply a column by the cell width, so a mis-measured glyph slides the
 * text out from under all three. */
static inline int uno_cp_width(int cp)
{
    if (cp < 0x0300) return 1;                            /* the common case */
    if ((cp >= 0x0300 && cp <= 0x036F) ||                 /* combining marks */
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x200B && cp <= 0x200F) ||                 /* zero-width      */
        (cp >= 0x20D0 && cp <= 0x20FF) ||                 /* combining marks */
        (cp >= 0xFE00 && cp <= 0xFE0F) ||                 /* variation sel.  */
        (cp >= 0xFE20 && cp <= 0xFE2F) ||
         cp == 0xFEFF) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||                 /* Hangul Jamo     */
        (cp >= 0x2E80 && cp <= 0x303E) ||                 /* CJK radicals..  */
        (cp >= 0x3041 && cp <= 0x33FF) ||                 /* kana, CJK marks */
        (cp >= 0x3400 && cp <= 0x4DBF) ||                 /* CJK ext A       */
        (cp >= 0x4E00 && cp <= 0x9FFF) ||                 /* CJK unified     */
        (cp >= 0xA000 && cp <= 0xA4CF) ||                 /* Yi              */
        (cp >= 0xAC00 && cp <= 0xD7A3) ||                 /* Hangul syllables*/
        (cp >= 0xF900 && cp <= 0xFAFF) ||                 /* CJK compat      */
        (cp >= 0xFE30 && cp <= 0xFE6F) ||                 /* CJK forms       */
        (cp >= 0xFF00 && cp <= 0xFF60) ||                 /* fullwidth forms */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1F9FF) ||               /* emoji           */
        (cp >= 0x20000 && cp <= 0x3FFFD)) return 2;       /* CJK ext B..     */
    return 1;
}

#endif /* UNO_UTF8_H */
