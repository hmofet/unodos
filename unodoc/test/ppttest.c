/* ===========================================================================
 * ppttest - the host gate for unodoc's .ppt lane (OFFICE97-PLAN §4 phase 5).
 *
 *   text FILE          every slide's text, for run_tests.py to compare
 *                      against LibreOffice's own extraction
 *   info FILE          slide count
 *   fuzz FILE SEED N   mutations through the container AND the presentation
 *                      reader.  Must never crash, never hang.
 *   wtest              write a deck, read it back with OUR reader, assert
 *                      slides, text and shapes all survive (phase 5c)
 *   wfile FILE         write the demo deck to disk for the LibreOffice half
 *                      of the gate - our reader agreeing with our writer only
 *                      proves they share a misunderstanding
 * ======================================================================== */
#include "unodoc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void um_set_alloc(void *(*a)(unsigned long), void (*f)(void *));

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

/* Same rule as the other two tests: sniff the container, so small.ppt and
 * small.pptx - one deck saved twice - run through identical checks. */
static ud_zip *g_zip;

static ud_ppt *open_ppt(const unsigned char *buf, long len, ud_src *src, ud_cfb **cc)
{
    ud_cfb *c;
    ud_src_mem(src, buf, len);
    *cc = 0;
    if (g_zip) { ud_zip_close(g_zip); g_zip = 0; }
    if (ud_sniff(src) == UD_C_ZIP) {
        g_zip = ud_zip_open(src);
        return g_zip ? ud_pptx_open(g_zip) : 0;
    }
    c = ud_cfb_open(src);
    *cc = c;
    return c ? ud_ppt_open(c) : 0;
}

int main(int argc, char **argv)
{
    ud_set_alloc(t_alloc, free);
    um_set_alloc(t_alloc, free);   /* a zip part is inflated by unomedia */

    if (argc >= 3 && (strcmp(argv[1], "text") == 0 || strcmp(argv[1], "info") == 0)) {
        long n = 0;
        unsigned char *b = slurp(argv[2], &n);
        ud_src src;
        ud_cfb *c;
        ud_ppt *p;
        int i;
        if (!b) { printf("ERR: cannot read %s\n", argv[2]); return 2; }
        p = open_ppt(b, n, &src, &c);
        if (!p) { printf("ERR: %s\n", ud_error()); free(b); ud_cfb_close(c); return 1; }
        if (strcmp(argv[1], "info") == 0) {
            ud_shape sh[256];
            long total = 0;
            for (i = 0; i < ud_ppt_slides(p); i++) {
                int ns = ud_ppt_slide_shapes(p, i, sh, 256), k;
                total += ns;
                for (k = 0; k < ns; k++)
                    printf("shape\tslide=%d\tkind=%d\tid=%ld\tgroup=%d\t"
                           "box=%ld,%ld,%ld,%ld\n", i, sh[k].kind, sh[k].id,
                           sh[k].group, sh[k].x0, sh[k].y0, sh[k].x1, sh[k].y1);
            }
            printf("slides=%d shapes=%ld\n", ud_ppt_slides(p), total);
        } else {
            for (i = 0; i < ud_ppt_slides(p); i++)
                fputs(ud_ppt_slide_text(p, i), stdout);
        }
        ud_ppt_close(p);
        ud_cfb_close(c);
        free(b);
        return 0;
    }
    /* The `x` verbs are the same deck through the OOXML serialiser. */
    if (argc >= 2 && (strcmp(argv[1], "wtest")  == 0 ||
                      strcmp(argv[1], "wxtest") == 0 ||
                      strcmp(argv[1], "wfile")  == 0 ||
                      strcmp(argv[1], "wxfile") == 0)) {
        int ooxml = argv[1][1] == 'x';
        /* The demo deck: two slides, multi-paragraph body, and one string
         * that forces the UTF-16 text atom (the euro sign is CP-1252 0x80,
         * which Latin-1 bytes cannot carry). */
        ud_pptw *w = ud_pptw_new();
        unsigned char *ppt;
        long n;
        int s1 = ud_pptw_slide(w), s2 = ud_pptw_slide(w), bad = 0;
        ud_pptw_title(w, s1, "Slide one title");
        ud_pptw_body (w, s1, "alpha line\nbeta line");
        ud_pptw_title(w, s2, "Slide two title");
        ud_pptw_body (w, s2, "wide caf\xe9 \x80 euro");
        ppt = ooxml ? ud_pptxw_save(w, &n) : ud_pptw_save(w, &n);
        ud_pptw_free(w);
        if (!ppt) { printf("FAILED: save: %s\n", ud_error()); return 1; }
        if (argv[1][ooxml ? 2 : 1] == 'f') {
            FILE *f = argc >= 3 ? fopen(argv[2], "wb") : 0;
            if (!f || fwrite(ppt, 1, (size_t)n, f) != (size_t)n) {
                printf("FAILED: cannot write %s\n", argc >= 3 ? argv[2] : "?");
                if (f) fclose(f);
                free(ppt);
                return 1;
            }
            fclose(f);
            printf("wrote %ld bytes\n", n);
            free(ppt);
            return 0;
        }
        {
            ud_src src;
            ud_cfb *c;
            ud_ppt *p = open_ppt(ppt, n, &src, &c);
            ud_shape sh[16];
            int ns;
            if (!p) { printf("FAILED: reopen: %s\n", ud_error()); free(ppt); ud_cfb_close(c); return 1; }
            if (ud_ppt_slides(p) != 2) { printf("FAILED: %d slides, wanted 2\n", ud_ppt_slides(p)); bad = 1; }
            if (!bad && !strstr(ud_ppt_slide_text(p, 0), "Slide one title"))
                { printf("FAILED: slide 1 lost its title\n"); bad = 1; }
            if (!bad && !strstr(ud_ppt_slide_text(p, 0), "beta line"))
                { printf("FAILED: slide 1 lost a body paragraph\n"); bad = 1; }
            if (!bad && !strstr(ud_ppt_slide_text(p, 1), "caf\xe9"))
                { printf("FAILED: the UTF-16 atom lost its CP-1252 accents\n"); bad = 1; }
            if (!bad && !strstr(ud_ppt_slide_text(p, 1), "\x80"))
                { printf("FAILED: the euro sign did not survive\n"); bad = 1; }
            /* Shapes are an Escher construct.  A .pptx has no drawing
               records at all - its text lives in the slide part - so the
               shape assertions apply to the binary form only, and pretending
               otherwise would be a test written to agree with itself. */
            if (!bad && !ooxml) {
                ns = ud_ppt_slide_shapes(p, 0, sh, 16);
                /* patriarch group + title box + body box */
                if (ns != 3) { printf("FAILED: slide 1 has %d shapes, wanted 3\n", ns); bad = 1; }
                else if (sh[1].kind != 202 || sh[2].kind != 202)
                    { printf("FAILED: textboxes read back kind %d/%d, wanted 202\n",
                             sh[1].kind, sh[2].kind); bad = 1; }
                else if (sh[1].y0 >= sh[2].y0)
                    { printf("FAILED: title anchor is not above the body anchor\n"); bad = 1; }
            }
            ud_ppt_close(p);
            ud_cfb_close(c);
        }
        free(ppt);
        if (!bad)
            printf("OK pptw: %s - 2 slides, text and both encodings%s "
                   "survive our own reader\n",
                   ooxml ? ".pptx" : ".ppt",
                   ooxml ? "" : " and 3 shapes");
        return bad;
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
            ud_ppt *p;
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
            p = open_ppt(m, n, &src, &c);
            if (p) {
                int i;
                opened++;
                for (i = 0; i < ud_ppt_slides(p); i++) {
                    ud_shape sh[64];
                    (void)strlen(ud_ppt_slide_text(p, i));
                    (void)ud_ppt_slide_shapes(p, i, sh, 64);
                }
                ud_ppt_close(p);
            }
            ud_cfb_close(c);
            free(m);
        }
        printf("OK %ld mutations, %ld presentations opened\n", iters, opened);
        free(b);
        return 0;
    }
    printf("usage: ppttest text FILE | info FILE | fuzz FILE SEED N\n");
    return 2;
}
