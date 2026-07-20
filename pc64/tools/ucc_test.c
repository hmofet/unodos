/* ===========================================================================
 * ucc_test - the host-side battery for the UnoC compiler.
 *
 * Each positive case is a UnoC snippet defining `long test(long a, long b)`.
 * The harness appends a `uno_app_main` wrapper (the module entry the emitter
 * requires), compiles the snippet with ucc, then LOADS THE .UNO ITSELF with
 * a reimplementation of pc64_modload.c's mod_load (mmap+PROT_EXEC, CRC,
 * rebase, named-import resolve against ms_abi host stubs) and calls the
 * entry through an ms_abi function pointer.  So every case exercises
 * lexer -> parser -> codegen -> container -> loader -> real execution,
 * including the Microsoft-x64 ABI boundary the pc64 kernel will use.
 *
 * Negative cases assert that a snippet fails with a diagnostic on the
 * expected line.
 *
 * Build (WSL/Linux):
 *   gcc -O1 -Wall -o ucc_test tools/ucc_test.c apps/ucc.c apps/ucc_x64.c \
 *       -DUCC_KEXPORTS_H='"../build/apps/ucc_kexports.h"'
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../apps/ucc.h"

#define MS __attribute__((ms_abi))

/* ---- ms_abi stubs standing in for the kernel exports ----------------------- */
static MS void *s_memcpy(void *d, const void *s, unsigned long n) { return memcpy(d, s, n); }
static MS void *s_memmove(void *d, const void *s, unsigned long n) { return memmove(d, s, n); }
static MS void *s_memset(void *d, int c, unsigned long n) { return memset(d, c, n); }
static MS int   s_memcmp(const void *a, const void *b, unsigned long n) { return memcmp(a, b, n); }
static MS unsigned long s_strlen(const char *s) { return strlen(s); }
static MS char *s_strcpy(char *d, const char *s) { return strcpy(d, s); }
static MS char *s_strncpy(char *d, const char *s, unsigned long n) { return strncpy(d, s, n); }
static MS char *s_strcat(char *d, const char *s) { return strcat(d, s); }
static MS int   s_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static MS int   s_strncmp(const char *a, const char *b, unsigned long n) { return strncmp(a, b, n); }
static MS long  s_TickCount(void) { return 60042; }
static MS short s_Random(void) { return 12345; }
static MS char *s_NewPtr(long n) { return malloc((size_t)n); }
static MS void  s_DisposePtr(char *p) { free(p); }

static const struct { const char *name; void *fn; } kStubs[] = {
    {"memcpy", (void *)s_memcpy}, {"memmove", (void *)s_memmove},
    {"memset", (void *)s_memset}, {"memcmp", (void *)s_memcmp},
    {"strlen", (void *)s_strlen}, {"strcpy", (void *)s_strcpy},
    {"strncpy", (void *)s_strncpy}, {"strcat", (void *)s_strcat},
    {"strcmp", (void *)s_strcmp}, {"strncmp", (void *)s_strncmp},
    {"TickCount", (void *)s_TickCount}, {"Random", (void *)s_Random},
    {"NewPtr", (void *)s_NewPtr}, {"DisposePtr", (void *)s_DisposePtr},
};

/* ---- host mod_load (mirrors pc64_modload.c checks 1:1) ---------------------- */
typedef struct {
    unsigned int magic;
    unsigned short abi, flags;
    unsigned int entry, mem_size, file_size, nreloc;
    unsigned int imp_rva, imp_count;
    unsigned long long pref_base;
    unsigned int crc, rsv;
} UnoModHdr;

static unsigned int mod_crc32(const unsigned char *p, long n)
{
    unsigned int c = 0xFFFFFFFFu; long i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static const char *g_loaderr;

static void *host_mod_load(const unsigned char *buf, long n)
{
    const UnoModHdr *h = (const UnoModHdr *)buf;
    unsigned char *base;
    const unsigned int *rel;
    unsigned int i;

    g_loaderr = 0;
    if (n < (long)sizeof *h)               { g_loaderr = "short";      return 0; }
    if (h->magic != 0x314F4E55u)           { g_loaderr = "bad magic";  return 0; }
    if (h->abi != 1)                       { g_loaderr = "bad abi";    return 0; }
    if ((long)sizeof *h + h->file_size + 4ll * h->nreloc != n)
                                           { g_loaderr = "bad size";   return 0; }
    if (h->entry >= h->mem_size || h->file_size > h->mem_size)
                                           { g_loaderr = "bad hdr";    return 0; }
    if (mod_crc32(buf + sizeof *h, n - (long)sizeof *h) != h->crc)
                                           { g_loaderr = "bad crc";    return 0; }

    base = mmap(0, (h->mem_size + 4095u) & ~4095u,
                PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)                { g_loaderr = "mmap";       return 0; }
    memcpy(base, buf + sizeof *h, h->file_size);
    memset(base + h->file_size, 0, h->mem_size - h->file_size);

    rel = (const unsigned int *)(buf + sizeof *h + h->file_size);
    for (i = 0; i < h->nreloc; i++) {
        if (rel[i] + 8u > h->mem_size)     { g_loaderr = "bad reloc";  return 0; }
        *(unsigned long long *)(base + rel[i]) +=
            (unsigned long long)base - h->pref_base;
    }
    for (i = 0; i < h->imp_count; i++) {
        char *rec = (char *)base + h->imp_rva + 32u * i;
        void *fn = 0;
        unsigned int j;
        if (h->imp_rva + 32u * (i + 1) > h->mem_size) { g_loaderr = "bad imp"; return 0; }
        rec[23] = 0;
        for (j = 0; j < sizeof kStubs / sizeof kStubs[0]; j++)
            if (!strcmp(kStubs[j].name, rec)) { fn = kStubs[j].fn; break; }
        if (!fn) { g_loaderr = rec; return 0; }
        *(unsigned long long *)(rec + 24) = (unsigned long long)fn;
    }
    return base + h->entry;
}

/* ---- the test driver ---------------------------------------------------------*/
typedef MS long (*TestFn)(long a, long b);

static long g_incn;
static long inc_read(void *ctx, const char *name, char *buf, long max)
{
    /* tests may register one in-memory include file */
    const char **inc = ctx;
    long n;
    if (!inc || !inc[0] || strcmp(name, inc[0])) return -1;
    n = (long)strlen(inc[1]);
    if (n > max) return -1;
    memcpy(buf, inc[1], (size_t)n);
    g_incn++;
    return n;
}

#define WRAP "\nlong uno_app_main(long a, long b) { return test(a, b); }\n"

static int g_pass, g_fail;
static unsigned char g_out[1 << 20];
static void *g_work;
#define WORKLEN (8L << 20)

static void run_case(const char *name, const char *src, long a, long b,
                     long expect, const char **inc)
{
    char full[65536];
    UccDiag diags[8];
    int nd = 0;
    long n;
    void *entry;
    long got;

    snprintf(full, sizeof full, "%s%s", src, WRAP);
    n = ucc_compile(full, (long)strlen(full), "TEST.C",
                    inc ? inc_read : 0, (void *)inc,
                    g_work, WORKLEN, g_out, sizeof g_out, diags, 8, &nd);
    if (n < 0) {
        printf("FAIL %-28s compile: %s (line %d)\n", name,
               nd ? diags[0].msg : "?", nd ? diags[0].line : 0);
        g_fail++;
        return;
    }
    entry = host_mod_load(g_out, n);
    if (!entry) {
        printf("FAIL %-28s load: %s\n", name, g_loaderr);
        g_fail++;
        return;
    }
    got = ((TestFn)entry)(a, b);
    if (got != expect) {
        printf("FAIL %-28s got %ld want %ld\n", name, got, expect);
        g_fail++;
        return;
    }
    g_pass++;
}

static void run_err(const char *name, const char *src, int wantline,
                    const char *wantmsg)
{
    char full[65536];
    UccDiag diags[8];
    int nd = 0;
    long n;

    snprintf(full, sizeof full, "%s%s", src, WRAP);
    n = ucc_compile(full, (long)strlen(full), "TEST.C", 0, 0,
                    g_work, WORKLEN, g_out, sizeof g_out, diags, 8, &nd);
    if (n >= 0 || !nd) {
        printf("FAIL %-28s expected an error, compiled fine\n", name);
        g_fail++;
        return;
    }
    if (wantline && diags[0].line != wantline) {
        printf("FAIL %-28s error on line %d, want %d (%s)\n", name,
               diags[0].line, wantline, diags[0].msg);
        g_fail++;
        return;
    }
    if (wantmsg && !strstr(diags[0].msg, wantmsg)) {
        printf("FAIL %-28s error '%s', want '%s'\n", name, diags[0].msg, wantmsg);
        g_fail++;
        return;
    }
    g_pass++;
}

#define T(name, expect, src)          run_case(name, src, 0, 0, expect, 0)
#define TAB(name, a, b, expect, src)  run_case(name, src, a, b, expect, 0)

int main(void)
{
    g_work = malloc(WORKLEN);
    if (!g_work) return 2;

    /* ---- integers + arithmetic ------------------------------------------ */
    T("return-42", 42, "long test(long a, long b) { return 42; }");
    T("add", 5, "long test(long a, long b) { return 2 + 3; }");
    T("arith-mix", 26, "long test(long a, long b) { return 2 * 3 + 4 * 5; }");
    T("paren", 70, "long test(long a, long b) { return 2 * (3 + 4) * 5; }");
    T("sub-neg", -4, "long test(long a, long b) { return 3 - 7; }");
    T("div", 6, "long test(long a, long b) { return 45 / 7; }");
    T("mod", 3, "long test(long a, long b) { return 45 % 7; }");
    T("neg-div", -6, "long test(long a, long b) { return -45 / 7; }");
    T("unary-minus", -10, "long test(long a, long b) { return -(4 + 6); }");
    T("bitops", 0x1234 ^ 0x00FF, "long test(long a, long b) { return 0x1234 ^ 0x00FF; }");
    T("and-or", (0x1234 & 0xFF0) | 0x8000,
      "long test(long a, long b) { return (0x1234 & 0xFF0) | 0x8000; }");
    T("shifts", (1 << 12) + (1024 >> 3),
      "long test(long a, long b) { return (1 << 12) + (1024 >> 3); }");
    T("bitnot", ~0x55, "long test(long a, long b) { return ~0x55; }");
    T("hex-char", 'A' + 010, "long test(long a, long b) { return 'A' + 010; }");
    TAB("params", 33, 44, 77, "long test(long a, long b) { return a + b; }");
    TAB("param-swap", 10, 3, 7, "long test(long a, long b) { return a - b; }");

    /* 32-bit int semantics: wraparound + unsigned compare */
    T("int-wrap", 1,
      "int test(long a, long b) { int x = 2147483647; x = x + 1;"
      " return x == -2147483647 - 1; }");
    T("uint-wrap", 1,
      "long test(long a, long b) { unsigned x = 1; x = x - 2;"
      " return x == 4294967295U; }");
    T("uint-cmp", 1,
      "long test(long a, long b) { unsigned x = 1; x = x - 2;"
      " return x > 1000000; }");
    T("int-div-neg", -3,
      "long test(long a, long b) { int x = -10; return x / 3; }");
    T("uns-shr", 0x7FFFFFFF,
      "long test(long a, long b) { unsigned x = 0xFFFFFFFE; return x >> 1; }");
    T("sgn-shr", -1,
      "long test(long a, long b) { int x = -2; return x >> 1; }");

    /* ---- comparisons + logic --------------------------------------------- */
    T("cmp-chain", 1, "long test(long a, long b) { return 3 < 5 == 1; }");
    T("cmp-ge", 1, "long test(long a, long b) { return 5 >= 5; }");
    T("cmp-gt", 0, "long test(long a, long b) { return 5 > 5; }");
    T("logand", 1, "long test(long a, long b) { return 2 && 3; }");
    T("logand-0", 0, "long test(long a, long b) { return 2 && 0; }");
    T("logor", 1, "long test(long a, long b) { return 0 || 3; }");
    T("lognot", 1, "long test(long a, long b) { return !0; }");
    TAB("shortcircuit", 5, 0, 5,
        "long g; long boom(void) { g = 99; return 1; }"
        "long test(long a, long b) { if (b && boom()) return 0; return a + g; }");
    T("ternary", 7, "long test(long a, long b) { return 1 ? 7 : 9; }");
    T("ternary-else", 9, "long test(long a, long b) { return 0 ? 7 : 9; }");

    /* ---- control flow ------------------------------------------------------ */
    T("if-else", 1, "long test(long a, long b) { if (3 > 2) return 1; return 2; }");
    T("while-sum", 55,
      "long test(long a, long b) { long s = 0; long i = 1;"
      " while (i <= 10) { s = s + i; i = i + 1; } return s; }");
    T("for-sum", 55,
      "long test(long a, long b) { long s = 0;"
      " for (long i = 1; i <= 10; i++) s += i; return s; }");
    T("do-while", 10,
      "long test(long a, long b) { long i = 0; do { i++; } while (i < 10); return i; }");
    T("break", 5,
      "long test(long a, long b) { long i;"
      " for (i = 0; i < 100; i++) if (i == 5) break; return i; }");
    T("continue", 25,
      "long test(long a, long b) { long s = 0;"
      " for (long i = 0; i < 10; i++) { if (i % 2 == 0) continue; s += i; } return s; }");
    T("nested-loop", 100,
      "long test(long a, long b) { long s = 0;"
      " for (long i = 0; i < 10; i++) for (long j = 0; j < 10; j++) s++; return s; }");
    TAB("switch", 3, 0, 30,
        "long test(long a, long b) { switch (a) {"
        " case 1: return 10; case 2: return 20; case 3: return 30;"
        " default: return -1; } }");
    TAB("switch-dflt", 9, 0, -1,
        "long test(long a, long b) { switch (a) {"
        " case 1: return 10; default: return -1; } }");
    TAB("switch-fall", 2, 0, 12,
        "long test(long a, long b) { long s = 0; switch (a) {"
        " case 2: s += 2; case 1: s += 10; break; case 5: s = 99; } return s; }");
    T("empty-stmt", 4, "long test(long a, long b) { ;;; return 4; }");

    /* ---- functions ---------------------------------------------------------- */
    T("call", 12,
      "long three(void) { return 3; }"
      "long test(long a, long b) { return three() * 4; }");
    T("call-args", 22,
      "long madd(long x, long y, long z) { return x * y + z; }"
      "long test(long a, long b) { return madd(4, 5, 2); }");
    T("call-8args", 36,
      "long s8(long p, long q, long r, long s, long t, long u, long v, long w)"
      " { return p + q + r + s + t + u + v + w; }"
      "long test(long a, long b) { return s8(1, 2, 3, 4, 5, 6, 7, 8); }");
    T("stack-args-order", 87654321,
      "long pick(long p, long q, long r, long s, long t, long u, long v, long w)"
      " { return w*10000000 + v*1000000 + u*100000 + t*10000 + s*1000 + r*100 + q*10 + p; }"
      "long test(long a, long b) { return pick(1, 2, 3, 4, 5, 6, 7, 8); }");
    T("recursion", 120,
      "long fact(long n) { if (n <= 1) return 1; return n * fact(n - 1); }"
      "long test(long a, long b) { return fact(5); }");
    T("fib", 55,
      "long fib(long n) { if (n < 2) return n; return fib(n-1) + fib(n-2); }"
      "long test(long a, long b) { return fib(10); }");
    T("fwd-proto", 9,
      "long later(long x);"
      "long test(long a, long b) { return later(3); }"
      "long later(long x) { return x * x; }");
    T("void-fn", 21,
      "long g; void bump(void) { g = g + 21; }"
      "long test(long a, long b) { bump(); return g; }");
    T("deep-call-align", 15,
      "long f1(long x) { return x + 1; }"
      "long f2(long x) { return f1(x) + f1(x + 1); }"
      "long test(long a, long b) { return f2(3) + f2(0) * 2; }");

    /* the ABI boundary: kernel-style import in an expression context */
    T("import-call", 60042,
      "long TickCount(void);"
      "long test(long a, long b) { return TickCount(); }");
    T("import-strlen", 5,
      "unsigned long strlen(const char *s);"
      "long test(long a, long b) { return strlen(\"hello\"); }");
    T("import-in-expr", 60052,
      "long TickCount(void);"
      "long test(long a, long b) { return 4 + TickCount() + 3 * 2; }");
    T("import-nested-args", 10,
      "int strcmp(const char *x, const char *y);"
      "unsigned long strlen(const char *s);"
      "long test(long a, long b) {"
      " return strlen(\"hi\") + (strcmp(\"aa\", \"ab\") < 0) * 8 - 0; }");

    /* ---- locals / assignment ops -------------------------------------------- */
    T("compound-ops", 30,
      "long test(long a, long b) { long x = 10;"
      " x += 5; x -= 1; x *= 4; x /= 2; x %= 100; x |= 16; x &= 63; x ^= 2;"
      " return x; }");
    T("compound-shifts", 24,
      "long test(long a, long b) { long x = 3; x <<= 4; x >>= 1; return x; }");
    T("preinc", 6, "long test(long a, long b) { long x = 5; return ++x; }");
    T("postinc", 5,
      "long test(long a, long b) { long x = 5; long y = x++; return y + (x == 6) - 1; }");
    T("postdec", 5,
      "long test(long a, long b) { long x = 5; long y = x--; return y + (x == 4) - 1; }");
    T("comma", 7, "long test(long a, long b) { long x; x = (1, 2, 7); return x; }");
    T("multi-decl", 6, "long test(long a, long b) { long x = 1, y = 2, z = 3; return x+y+z; }");
    T("shadow", 3,
      "long test(long a, long b) { long x = 1; { long x = 2; } return x + 2; }");
    T("char-arith", 3,
      "long test(long a, long b) { char c = 'a'; char d = 'd'; return d - c; }");
    T("uchar-wrap", 44,
      "long test(long a, long b) { unsigned char c = 200; c += 100; return c; }");
    T("short-trunc", -6,
      "long test(long a, long b) { short s = 65530; return s; }");

    printf("\nucc_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
