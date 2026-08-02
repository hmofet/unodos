/* ===========================================================================
 * doctest - the host gate for unodoc's .doc reader (OFFICE97-PLAN §4 phase 4).
 *
 *   text FILE          the document's reading text, for run_tests.py to
 *                      compare against LibreOffice's own text conversion
 *   info FILE          pieces, character count, and the raw control-character
 *                      histogram - what the piece table actually produced
 *   fuzz FILE SEED N   mutations through the container AND the document
 *                      reader.  Must never crash, never hang.
 * ======================================================================== */
#include "unodoc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *t_alloc(unsigned long n) { return malloc(n ? n : 1); }

static unsigned char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    *len = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    b = (unsigned char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *len = n;
    return b;
}

static ud_doc *open_doc(const unsigned char *buf, long len, ud_src *src, ud_cfb **cc)
{
    ud_cfb *c;
    ud_src_mem(src, buf, len);
    c = ud_cfb_open(src);
    *cc = c;
    return c ? ud_doc_open(c) : 0;
}

int main(int argc, char **argv)
{
    ud_set_alloc(t_alloc, free);

    if (argc >= 3 && (strcmp(argv[1], "text") == 0 || strcmp(argv[1], "info") == 0)) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        ud_src src;
        ud_cfb *c;
        ud_doc *d;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        d = open_doc(b, n, &src, &c);
        if (!d) { printf("ERR: %s\n", ud_error()); free(b); ud_cfb_close(c); return 1; }
        if (strcmp(argv[1], "text") == 0) {
            fputs(ud_doc_plain(d), stdout);
        } else {
            const char *t = ud_doc_text(d);
            long i, ctrl = 0, para = 0, cell = 0, field = 0;
            for (i = 0; i < ud_doc_text_len(d); i++) {
                unsigned char ch = (unsigned char)t[i];
                if (ch == 0x0D) para++;
                else if (ch == 0x07) cell++;
                else if (ch == 0x13) field++;
                else if (ch < 0x20 && ch != '\t') ctrl++;
            }
            printf("pieces=%d chars=%ld paragraphs=%ld cells=%ld fields=%ld "
                   "other-controls=%ld\n",
                   ud_doc_pieces(d), ud_doc_text_len(d), para, cell, field, ctrl);
        }
        ud_doc_close(d);
        ud_cfb_close(c);
        free(b);
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "fuzz") == 0) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        unsigned seed = (unsigned)strtoul(argv[3], 0, 10);
        long iters = strtol(argv[4], 0, 10), t, opened = 0;
        if (!b || n < 512) { printf("ERR: cannot read %s\n", argv[2]); free(b); return 2; }
        for (t = 0; t < iters; t++) {
            unsigned char *m = (unsigned char *)malloc((size_t)n);
            ud_src src;
            ud_cfb *c;
            ud_doc *d;
            int k, nmut;
            memcpy(m, b, (size_t)n);
            seed = seed * 1103515245u + 12345u;
            nmut = (int)((seed >> 16) % 12u) + 1;
            for (k = 0; k < nmut; k++) {
                long off;
                seed = seed * 1103515245u + 12345u;
                off = (long)((seed >> 8) % (unsigned long)n);
                seed = seed * 1103515245u + 12345u;
                m[off] = (unsigned char)(seed >> 16);
            }
            d = open_doc(m, n, &src, &c);
            if (d) { opened++; (void)strlen(ud_doc_plain(d)); ud_doc_close(d); }
            ud_cfb_close(c);
            free(m);
        }
        printf("OK %ld mutations, %ld documents opened\n", iters, opened);
        free(b);
        return 0;
    }
    printf("usage: doctest text FILE | info FILE | fuzz FILE SEED N\n");
    return 2;
}
