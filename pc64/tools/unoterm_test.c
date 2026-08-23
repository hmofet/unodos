/* ===========================================================================
 * Host-side test for unoterm, the VT emulator.  No device, no network, no
 * framebuffer: build it on the dev PC and assert the resulting cell grid.
 *
 *     cc -O2 -Wall -Wextra -I.. -o unoterm_test unoterm_test.c ../unoterm.c
 *     ./unoterm_test
 *
 * IT BUILDS THE SAME FILE THE OS DOES.  That is the whole point and it is not
 * a detail: a host harness that reimplements or shims the thing it is testing
 * tests a different program, and this tree has paid for that lesson more than
 * once.  unoterm.c includes nothing, so there is nothing to shim.
 * ======================================================================== */
#include "../unoterm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail;

static void feed(unoterm *t, const char *s)
{ unoterm_feed(t, (const unsigned char *)s, (int)strlen(s)); }

static void expect_line(unoterm *t, int y, const char *want, const char *what)
{
    char got[256];
    unoterm_line_text(t, y, got, (int)sizeof got);
    if (strcmp(got, want) != 0) {
        printf("FAIL %-28s line %d: want \"%s\" got \"%s\"\n", what, y, want, got);
        g_fail++;
    } else {
        printf("ok   %-28s line %d\n", what, y);
    }
}

static void expect_int(int got, int want, const char *what)
{
    if (got != want) { printf("FAIL %-28s want %d got %d\n", what, want, got); g_fail++; }
    else printf("ok   %-28s = %d\n", what, got);
}

int main(void)
{
    static unsigned char mem[1 << 20];
    unoterm T;

    if (!unoterm_init(&T, mem, sizeof mem, 40, 6, 20)) {
        printf("FAIL init\n");
        return 1;
    }

    /* ---- the boring case, which is the one everything else rests on ---- */
    feed(&T, "hello world\r\nsecond line\r\n");
    expect_line(&T, 0, "hello world", "plain text");
    expect_line(&T, 1, "second line", "plain text");
    expect_int(T.cy, 2, "cursor after two lines");
    expect_int(T.cx, 0, "cursor column after CR");

    /* ---- CR without LF overwrites, which is how every progress bar works,
     * and is exactly what a flat scroll buffer cannot represent ---------- */
    feed(&T, "  0%\r 50%\r100%");
    expect_line(&T, 2, "100%", "CR overwrite (progress bar)");

    /* ---- cursor addressing + erase --------------------------------------- */
    feed(&T, "\033[2J\033[H");
    expect_line(&T, 0, "", "ED 2 cleared the screen");
    expect_int(T.cx + T.cy, 0, "CUP homed the cursor");

    feed(&T, "abcdefgh\033[1;4H");     /* row 1, col 4 */
    expect_int(T.cx, 3, "CUP column");
    feed(&T, "XY");
    expect_line(&T, 0, "abcXYfgh", "overwrite mid-line");

    feed(&T, "\033[1;4H\033[K");
    expect_line(&T, 0, "abc", "EL 0 erased to end of line");

    /* ---- insert / delete characters -------------------------------------- */
    feed(&T, "\033[2J\033[Habcdef\033[1;3H\033[2P");
    expect_line(&T, 0, "abef", "DCH deleted two characters");
    feed(&T, "\033[1;3H\033[2@");
    expect_line(&T, 0, "ab  ef", "ICH inserted two blanks");

    /* ---- SGR ------------------------------------------------------------- */
    feed(&T, "\033[2J\033[H\033[31;1mR\033[0m\033[38;5;120mP\033[38;2;10;20;30mT\033[m.");
    expect_int(unoterm_cell_at(&T, 0, 0)->fg, 1, "SGR 31 -> red");
    expect_int(unoterm_cell_at(&T, 0, 0)->attr & UNOTERM_BOLD, UNOTERM_BOLD, "SGR 1 -> bold");
    expect_int(unoterm_cell_at(&T, 1, 0)->fg, 120, "SGR 38;5;n -> palette");
    expect_int(unoterm_cell_at(&T, 2, 0)->fg,
               UNOTERM_RGB_FLAG | (10 << 16) | (20 << 8) | 30, "SGR 38;2 -> truecolour");
    expect_int(unoterm_cell_at(&T, 3, 0)->fg, UNOTERM_DEFAULT_COLOR, "SGR 0 reset");

    /* ---- scrolling and scrollback ---------------------------------------- */
    {
        int i;
        unoterm_init(&T, mem, sizeof mem, 40, 4, 20);
        for (i = 1; i <= 8; i++) { char b[16]; sprintf(b, "L%d\r\n", i); feed(&T, b); }
        /* 8 lines through a 4-row screen: 1..5 scrolled off, 5..8 visible with
         * the cursor parked on the last blank row. */
        expect_line(&T, 0, "L6", "after scrolling, top line");
        expect_line(&T, 2, "L8", "after scrolling, last text line");
        expect_int(unoterm_scrollback_count(&T), 5, "scrollback line count");
        {
            const unoterm_cell *sb = unoterm_scrollback(&T, 1);
            printf("ok   scrollback[1] first char = '%c'\n", sb ? (char)sb[0].ch : '?');
            if (!sb || sb[0].ch != 'L' || sb[1].ch != '5') {
                printf("FAIL scrollback[1] should be L5\n"); g_fail++;
            }
        }
    }

    /* ---- scroll region: the thing `less` and `vim` live inside ----------- */
    unoterm_init(&T, mem, sizeof mem, 20, 5, 8);
    feed(&T, "A\r\nB\r\nC\r\nD\r\nE");
    feed(&T, "\033[2;4r");                     /* region = rows 2..4         */
    expect_int(T.cy, 1, "DECSTBM homes into the region");
    feed(&T, "\033[4;1H\r\nX");                /* force a scroll inside it   */
    expect_line(&T, 0, "A", "row above the region is untouched");
    expect_line(&T, 4, "E", "row below the region is untouched");
    expect_line(&T, 3, "X", "the region scrolled");

    /* ---- the alternate screen: enter, scribble, leave, and the primary
     * must be exactly as it was.  This is the one that makes `vim` usable. -- */
    unoterm_init(&T, mem, sizeof mem, 20, 4, 8);
    feed(&T, "keep me\r\nand me");
    feed(&T, "\033[?1049h");
    expect_line(&T, 0, "", "alt screen starts blank");
    feed(&T, "throwaway");
    expect_line(&T, 0, "throwaway", "alt screen takes text");
    feed(&T, "\033[?1049l");
    expect_line(&T, 0, "keep me", "primary survived the alt screen");
    expect_line(&T, 1, "and me", "primary survived the alt screen");

    /* ---- UTF-8 split across two feeds, the normal case on a stream ------- */
    unoterm_init(&T, mem, sizeof mem, 20, 2, 4);
    unoterm_feed(&T, (const unsigned char *)"\xc3", 1);
    unoterm_feed(&T, (const unsigned char *)"\xa9", 1);
    expect_int(unoterm_cell_at(&T, 0, 0)->ch, 0xE9, "UTF-8 split across feeds");
    /* a stray continuation byte must not poison what follows */
    unoterm_feed(&T, (const unsigned char *)"\x80" "Z", 2);
    expect_int(unoterm_cell_at(&T, 1, 0)->ch, 'Z', "stray continuation resyncs");

    /* ---- a garbled repeat count must not spin ---------------------------- */
    unoterm_init(&T, mem, sizeof mem, 20, 4, 4);
    feed(&T, "\033[99999999S");
    printf("ok   huge repeat count returned\n");

    /* ---- OSC: title and working directory -------------------------------- */
    unoterm_init(&T, mem, sizeof mem, 20, 4, 4);
    feed(&T, "\033]0;my title\007");
    if (strcmp(T.title, "my title")) { printf("FAIL OSC 0 title: \"%s\"\n", T.title); g_fail++; }
    else printf("ok   OSC 0 title\n");
    feed(&T, "\033]7;file://box/srv/media\033\\");
    if (strcmp(T.cwd, "/srv/media")) { printf("FAIL OSC 7 cwd: \"%s\"\n", T.cwd); g_fail++; }
    else printf("ok   OSC 7 working directory\n");
    /* ST is ESC backslash: the backslash must be CONSUMED, not printed */
    expect_line(&T, 0, "", "ST did not leak a backslash");

    /* ---- keys ------------------------------------------------------------ */
    {
        char k[16];
        int n;
        unoterm_init(&T, mem, sizeof mem, 20, 4, 4);
        n = unoterm_key(&T, UNOTERM_K_UP, 0, 0, k, sizeof k);
        if (n != 3 || memcmp(k, "\033[A", 3)) { printf("FAIL normal cursor key\n"); g_fail++; }
        else printf("ok   cursor key (normal mode)\n");
        feed(&T, "\033[?1h");
        n = unoterm_key(&T, UNOTERM_K_UP, 0, 0, k, sizeof k);
        if (n != 3 || memcmp(k, "\033OA", 3)) { printf("FAIL application cursor key\n"); g_fail++; }
        else printf("ok   cursor key (application mode)\n");
        n = unoterm_key(&T, 'c', 1, 0, k, sizeof k);
        if (n != 1 || k[0] != 3) { printf("FAIL ctrl-C\n"); g_fail++; }
        else printf("ok   ctrl-C is 0x03\n");
        n = unoterm_key(&T, 0xE9, 0, 0, k, sizeof k);
        if (n != 2 || (unsigned char)k[0] != 0xC3 || (unsigned char)k[1] != 0xA9) {
            printf("FAIL non-ASCII key encodes UTF-8\n"); g_fail++;
        } else printf("ok   non-ASCII key encodes UTF-8\n");
    }

    /* ---- bracketed paste ------------------------------------------------- */
    {
        char out[64];
        int n;
        unoterm_init(&T, mem, sizeof mem, 20, 4, 4);
        n = unoterm_paste(&T, "a\nb", 3, out, sizeof out);
        if (n != 3 || memcmp(out, "a\nb", 3)) { printf("FAIL plain paste\n"); g_fail++; }
        else printf("ok   plain paste is verbatim\n");
        feed(&T, "\033[?2004h");
        n = unoterm_paste(&T, "a\nb", 3, out, sizeof out);
        if (n != 15 || memcmp(out, "\033[200~a\nb\033[201~", 15)) {
            printf("FAIL bracketed paste\n"); g_fail++;
        } else printf("ok   bracketed paste is wrapped\n");
    }

    /* ---- init must REFUSE a block that is too small, not scribble past it */
    {
        static unsigned char tiny[64];
        if (unoterm_init(&T, tiny, sizeof tiny, 200, 60, 500)) {
            printf("FAIL init accepted a block that is too small\n"); g_fail++;
        } else printf("ok   init refuses an undersized block\n");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
