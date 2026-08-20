/* ===========================================================================
 * ud_ooxz.h - the package plumbing the three OOXML writers share.  Internal;
 * not part of the contract.
 *
 * WHY THIS IS ITS OWN FILE.  All three serialisers write the same container -
 * a stored zip of XML parts, with the same escaping, the same numbers and the
 * same `_rels/.rels` - and only the parts inside differ.  Putting all three in
 * one translation unit was the first shape of this and it was wrong for a
 * reason worth writing down: an app that only reads and writes spreadsheets
 * would have linked the .docx and .pptx serialisers too, and the .doc/.ppt
 * models with them.  One file per format keeps a linker able to leave out what
 * a program does not use, which on a loadable module is the difference between
 * an app that fits the arena and one that does not.
 * ======================================================================== */
#ifndef UD_OOXZ_H
#define UD_OOXZ_H

/* ---- a growable byte buffer ------------------------------------------------ */
typedef struct { unsigned char *p; long n, cap; int bad; } zbuf;

void zput(zbuf *b, const void *d, long n);
void zs  (zbuf *b, const char *s);
void z8  (zbuf *b, unsigned v);
void z16 (zbuf *b, unsigned v);
void z32 (zbuf *b, unsigned long v);
void zint(zbuf *b, long v);
void znum(zbuf *b, double v);

/* XML text: CP-1252 in (unodoc's models hold nothing else), UTF-8 out, with
 * the markup characters escaped.  `quot` also escapes `"`, for an attribute. */
void zxml(zbuf *b, const char *s, int quot);

/* ---- the stored-zip writer ------------------------------------------------- */
#define UD_OOXZ_MAXPART 512

typedef struct {
    zbuf  out;
    struct { long off; unsigned long crc, size; char name[128]; } e[UD_OOXZ_MAXPART];
    int   n;
} zwrite;

/* Append `body` as a part named `name`.  TAKES the buffer: it is freed here,
 * and the caller's zbuf is left zeroed so it can be reused for the next
 * part. */
void zw_part(zwrite *z, const char *name, zbuf *body);

/* Write the central directory and hand back the package in one ud_alloc'd
 * block, or NULL with ud_error() set.  The zwrite is left zeroed either way. */
unsigned char *zw_finish(zwrite *z, long *len);

/* `<?xml ...?>`, which every part starts with. */
void xml_head(zbuf *b);

/* The package-level `_rels/.rels`: the same three lines for all three formats
 * bar the part it points at. */
void root_rels(zwrite *z, const char *target);

/* Append "sheet12.xml" style names without a printf. */
void zw_name(char *out, const char *prefix, int n, const char *suffix);

#define NS_R  "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_CT "http://schemas.openxmlformats.org/package/2006/content-types"
#define NS_PR "http://schemas.openxmlformats.org/package/2006/relationships"
#define RT    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
#define ML    "http://schemas.openxmlformats.org/"

#endif /* UD_OOXZ_H */
