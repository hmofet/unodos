/* ===========================================================================
 * uc_test.c - host tests for UnoCode's two pure-logic foundations.
 *
 * uc_json.c and uc_rx.c have no framebuffer, no toolkit and no filesystem in
 * them, so they can be compiled and exercised on the build host.  That matters
 * more here than usual: EVERY config file, theme, keymap, extension manifest
 * and syntax grammar in UnoCode arrives through these two files, so a bug in
 * either shows up as "themes stopped loading" three layers away from the cause.
 *
 *   cd pc64/unocode && sh tools/test.sh
 * ======================================================================== */
#include <stdio.h>
#include <string.h>
#include "unocode.h"

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL  %s\n", what); }
}

/* ---- JSON ---------------------------------------------------------------- */
static const char *kSettings =
    "// UnoCode settings\n"
    "{\n"
    "    \"editor.fontSize\": 14,\n"
    "    \"editor.tabSize\": 4,\n"
    "    \"editor.insertSpaces\": true,\n"
    "    /* block comment\n"
    "       spanning lines */\n"
    "    \"workbench.colorTheme\": \"Dark+ (default dark)\",\n"
    "    \"editor\": { \"minimap\": { \"enabled\": false } },\n"
    "    \"files.exclude\": [\"*.UNO\", \"*.O\",],\n"
    "    \"escapes\": \"a\\tb\\n\\\"q\\\"\\u00e9\",\n"
    "}\n";

static void test_json(void)
{
    char err[80];
    UcJson *r = uc_json_parse(kSettings, -1, err, sizeof err), *m;
    printf("json:\n");
    ck(r != 0, "settings parse");
    if (!r) { printf("  err: %s\n", err); return; }
    ck(r->type == UJ_OBJ, "root is an object");
    ck((int)uc_json_num(r, "editor.fontSize", 0) == 14, "flat dotted number");
    ck(uc_json_bool(r, "editor.insertSpaces", 0) == 1, "flat dotted bool");
    ck(!strcmp(uc_json_str(r, "workbench.colorTheme", ""), "Dark+ (default dark)"),
       "string value");
    m = uc_json_path(r, "editor.minimap.enabled");
    ck(m && m->type == UJ_BOOL && m->bval == 0, "nested dotted path");
    m = uc_json_member(r, "files.exclude");
    ck(m && m->type == UJ_ARR && m->n == 2, "trailing comma in an array");
    ck(m && uc_json_at(m, 1) && !strcmp(uc_json_at(m, 1)->str, "*.O"), "array index");
    m = uc_json_member(r, "escapes");
    ck(m && m->str && m->str[1] == '\t' && m->str[3] == '\n' && m->str[4] == '"',
       "string escapes");
    ck(m && (unsigned char)m->str[7] == 0xC3 && (unsigned char)m->str[8] == 0xA9,
       "\\u00e9 becomes two UTF-8 bytes");
    uc_json_free(r);

    r = uc_json_parse("{ \"a\": 1, ", -1, err, sizeof err);
    ck(r == 0 && err[0], "unterminated object is an error, with a message");

    r = uc_json_parse("[1, 2.5, -3e2, true, null]", -1, err, sizeof err);
    ck(r && r->n == 5, "array of scalars");
    if (r) {
        ck(uc_json_at(r, 1)->num == 2.5, "fraction");
        ck(uc_json_at(r, 2)->num == -300.0, "exponent");
        uc_json_free(r);
    }
}

/* ---- regex --------------------------------------------------------------- */
static void rx_case(const char *pat, const char *sub, int expect,
                    const char *want)
{
    char err[80];
    int caps[UC_RX_CAPS * 2];
    UcRx *rx = uc_rx_compile(pat, 0, err, sizeof err);
    char label[160];
    int hit;
    snprintf(label, sizeof label, "/%s/ on \"%s\"", pat, sub);
    if (!rx) { checks++; fails++; printf("  FAIL  %s -> compile: %s\n", label, err); return; }
    hit = uc_rx_exec(rx, sub, (int)strlen(sub), 0, 1, caps);
    checks++;
    if (hit != expect) {
        fails++;
        printf("  FAIL  %s -> %s, wanted %s\n", label, hit ? "match" : "no match",
               expect ? "match" : "no match");
    } else if (hit && want) {
        int n = caps[1] - caps[0];
        checks++;
        if (n != (int)strlen(want) || strncmp(sub + caps[0], want, (size_t)n)) {
            fails++;
            printf("  FAIL  %s -> matched \"%.*s\", wanted \"%s\"\n",
                   label, n, sub + caps[0], want);
        }
    }
    uc_rx_free(rx);
}

static void rx_group(const char *pat, const char *sub, int grp, const char *want)
{
    char err[80];
    int caps[UC_RX_CAPS * 2];
    UcRx *rx = uc_rx_compile(pat, 0, err, sizeof err);
    checks++;
    if (!rx) { fails++; printf("  FAIL  /%s/ compile: %s\n", pat, err); return; }
    if (!uc_rx_exec(rx, sub, (int)strlen(sub), 0, 1, caps)) {
        fails++; printf("  FAIL  /%s/ on \"%s\": no match\n", pat, sub);
    } else {
        int a = caps[grp * 2], b = caps[grp * 2 + 1];
        if (a < 0 || b < a || (int)strlen(want) != b - a ||
            strncmp(sub + a, want, (size_t)(b - a))) {
            fails++;
            printf("  FAIL  /%s/ group %d = \"%.*s\", wanted \"%s\"\n",
                   pat, grp, a < 0 ? 0 : b - a, a < 0 ? "" : sub + a, want);
        }
    }
    uc_rx_free(rx);
}

static void test_rx(void)
{
    char err[80];
    UcRx *rx;
    int caps[UC_RX_CAPS * 2];
    printf("regex:\n");

    rx_case("abc", "xxabcyy", 1, "abc");
    rx_case("^abc", "xxabc", 0, 0);
    rx_case("abc$", "xxabc", 1, "abc");
    rx_case("a.c", "a\nc", 0, 0);
    rx_case("a.c", "abc", 1, "abc");
    rx_case("a*", "aaab", 1, "aaa");
    rx_case("a*?b", "aaab", 1, "aaab");
    rx_case("a+", "baaa", 1, "aaa");
    rx_case("ab?c", "ac", 1, "ac");
    rx_case("ab?c", "abc", 1, "abc");
    rx_case("[a-cx]+", "zzabcxq", 1, "abcx");
    rx_case("[^a-z]+", "abc123def", 1, "123");
    rx_case("\\d{2,4}", "x12345", 1, "1234");
    rx_case("\\d{3}", "x12y", 0, 0);
    rx_case("a{2,}", "xaaaa", 1, "aaaa");
    rx_case("a{2,}", "xa", 0, 0);
    rx_case("a{0,2}b", "b", 1, "b");
    rx_case("(cat|dog|bird)s?", "two dogs", 1, "dogs");
    rx_case("\\bword\\b", "a word here", 1, "word");
    rx_case("\\bword\\b", "sword", 0, 0);
    rx_case("\\w+", "  hello_9 ", 1, "hello_9");
    rx_case("\\s+", "ab  cd", 1, "  ");
    rx_case("//.*", "int x; // note", 1, "// note");
    rx_case("\"(\\\\.|[^\"\\\\])*\"", "s = \"a\\\"b\" ;", 1, "\"a\\\"b\"");
    rx_case("0[xX][0-9a-fA-F]+", "v = 0xDEAD;", 1, "0xDEAD");
    rx_case("^\\s*#\\s*(include|define)\\b", "  # include <x>", 1, "  # include");
    rx_case("(?:ab)+", "xababy", 1, "abab");

    rx_group("(\\w+)\\s*=\\s*(\\d+)", "count = 42", 1, "count");
    rx_group("(\\w+)\\s*=\\s*(\\d+)", "count = 42", 2, "42");
    rx_group("(a)(b)?(c)", "ac", 3, "c");

    /* case folding */
    rx = uc_rx_compile("hello", 1, err, sizeof err);
    checks++;
    if (!rx || !uc_rx_exec(rx, "say HeLLo", 9, 0, 1, caps)) {
        fails++; printf("  FAIL  case-insensitive literal\n");
    }
    uc_rx_free(rx);
    rx = uc_rx_compile("[a-f]+", 1, err, sizeof err);
    checks++;
    if (!rx || !uc_rx_exec(rx, "ZZBCD", 5, 0, 1, caps) || caps[0] != 2) {
        fails++; printf("  FAIL  case-insensitive class\n");
    }
    uc_rx_free(rx);

    /* the two constructs we refuse rather than mis-match */
    ck(uc_rx_compile("(?=x)", 0, err, sizeof err) == 0, "lookahead is rejected");
    ck(uc_rx_compile("(a)\\1", 0, err, sizeof err) == 0, "backreference is rejected");

    /* a pathological pattern must return, not hang */
    rx = uc_rx_compile("(a+)+b", 0, err, sizeof err);
    checks++;
    if (rx) {
        uc_rx_exec(rx, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 30, 0, 1, caps);
        uc_rx_free(rx);
        printf("  ok    catastrophic pattern returned\n");
    } else { fails++; printf("  FAIL  (a+)+b did not compile\n"); }
}

int main(void)
{
    test_json();
    test_rx();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
