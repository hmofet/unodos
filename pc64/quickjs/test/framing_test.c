/* framing_test.c - where does a response END?
 *
 * The two framings that matter (Content-Length and chunked) decide when the
 * browser stops reading. Get it wrong in one direction and a page is
 * truncated; get it wrong in the other and the browser hangs waiting for
 * bytes that will never come. Both failures are worth a test each.
 *
 * The framing helpers are static in pc64_http.c, which is full of NIC and
 * TLS dependencies, so this includes the two functions directly rather than
 * linking the whole transport - they are self-contained string code and
 * that is exactly what is under test.
 */
#include <stdio.h>
#include <string.h>

/* ---- the code under test, verbatim from pc64_http.c ---------------------- */
static int hdr_split(const char *raw, int rn)
{
    int i;
    for (i = 0; i + 1 < rn; i++) {
        if (i + 3 < rn && raw[i]=='\r' && raw[i+1]=='\n' && raw[i+2]=='\r' && raw[i+3]=='\n')
            return i + 4;
        if (raw[i]=='\n' && raw[i+1]=='\n') return i + 2;
    }
    return -1;
}

static int dechunk(char *body, int len)
{
    int in = 0, out = 0;
    for (;;) {
        long sz = 0;
        int digits = 0;
        while (in < len && (body[in]=='\r' || body[in]=='\n')) in++;
        while (in < len) {
            char c = body[in];
            int v = (c>='0'&&c<='9') ? c-'0' :
                    (c>='a'&&c<='f') ? c-'a'+10 :
                    (c>='A'&&c<='F') ? c-'A'+10 : -1;
            if (v < 0) break;
            sz = sz * 16 + v;
            in++; digits++;
        }
        if (!digits) return -1;
        while (in < len && body[in] != '\n') in++;
        if (in >= len) return -1;
        in++;
        if (sz == 0) { body[out] = 0; return out; }
        if (in + sz > len) return -1;
        memmove(body + out, body + in, (size_t)sz);
        out += (int)sz;
        in += (int)sz;
    }
}

/* ---- checks -------------------------------------------------------------- */
static int g_pass, g_fail;
static void note(int ok, const char *name)
{ printf("%s %s\n", ok ? "pass" : "FAIL", name); if (ok) g_pass++; else g_fail++; }

static int split_of(const char *s) { return hdr_split(s, (int)strlen(s)); }

int main(void)
{
    char buf[512];

    /* the header/body split */
    /* "HTTP/1.1 200 OK" is 15 chars (0..14), CRLF 15..16, "A: b" 17..20,
     * CRLF 21..22, the blank line's CRLF 23..24 - so the body starts at 25. */
    note(split_of("HTTP/1.1 200 OK\r\nA: b\r\n\r\nBODY") == 25, "CRLFCRLF split");
    note(split_of("HTTP/1.1 200 OK\nA: b\n\nBODY") == 22, "bare LFLF split");
    note(split_of("HTTP/1.1 200 OK\r\nA: b\r\n") < 0, "incomplete headers: keep reading");
    note(split_of("") < 0, "empty: keep reading");

    /* chunked decoding */
    strcpy(buf, "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
    note(dechunk(buf, (int)strlen(buf)) == 9 && !strcmp(buf, "Wikipedia"),
         "chunked: two chunks joined");

    strcpy(buf, "0\r\n\r\n");
    note(dechunk(buf, (int)strlen(buf)) == 0 && !buf[0], "chunked: empty body");

    strcpy(buf, "a\r\n0123456789\r\n0\r\n\r\n");
    note(dechunk(buf, (int)strlen(buf)) == 10 && !strcmp(buf, "0123456789"),
         "chunked: hex size (a = 10)");

    strcpy(buf, "4;ext=1\r\nWiki\r\n0\r\n\r\n");
    note(dechunk(buf, (int)strlen(buf)) == 4 && !strcmp(buf, "Wiki"),
         "chunked: chunk extension ignored");

    /* the truncation cases: dechunk must say "not yet", never guess. These
     * are what stop a partial response being rendered as if complete. */
    strcpy(buf, "4\r\nWiki\r\n5\r\npedia\r\n");
    note(dechunk(buf, (int)strlen(buf)) == -1, "chunked: no terminator -> incomplete");

    strcpy(buf, "9\r\nWiki");
    note(dechunk(buf, (int)strlen(buf)) == -1, "chunked: chunk cut short -> incomplete");

    strcpy(buf, "4\r\nWiki\r\n5");
    note(dechunk(buf, (int)strlen(buf)) == -1, "chunked: size line cut short -> incomplete");

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
