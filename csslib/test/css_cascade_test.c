/* css_cascade_test.c - the CS2 host test: libcss AS unoweb's cascade.
 *
 * Parses a real document with unoweb, styles it twice - once with the
 * built-in cascade, once with the libcss bridge registered - and asserts
 * (a) the bridge produces the expected computed styles for selector shapes
 * the built-in matcher also supports, (b) the two cascades AGREE on the
 * core fields for every element (the parity that lets the browser switch
 * between them), and (c) libcss-only ground (attribute selectors) works.
 *
 * Build via build-host-test.sh (it links unoweb's objects alongside the
 * CSS stack; both are portable freestanding C).
 */
#include <stdio.h>
#include <string.h>
#include "../uw_bridge.h"

static int g_pass, g_fail;

static void note(int ok, const char *name)
{ printf("%s %s\n", ok ? "pass" : "FAIL", name); if (ok) g_pass++; else g_fail++; }

static const char HTML[] =
    "<html><head><title>t</title>"
    "<style>"
    ".hot { color: #c81e28; font-weight: bold } "
    "#lead { font-size: 20px; margin: 12px 0 } "
    "ul li { color: #285a1e } "
    "p { text-align: center } "
    "a[href] { white-space: nowrap } "
    "</style></head>"
    "<body>"
    "<h1>Title</h1>"
    "<p id='lead' class='hot'>lead paragraph</p>"
    "<p style='color: #123456'>inline styled</p>"
    "<ul><li>one</li><li>two</li></ul>"
    "<a href='x'>link</a>"
    "<pre>mono</pre>"
    "</body></html>";

static uw_node *nth_tag(uw_doc *d, const char *tag, int i)
{
    uw_node *els[16];
    int n = uw_elements_by_tag(d, uw_document(d), tag, els, 16);
    return i < n ? els[i] : NULL;
}

static void check_expectations(uw_doc *d, const char *who)
{
    char name[96];
    const uw_style *st;

#define N(field) (snprintf(name, sizeof name, "%s: %s", who, field), name)

    st = uw_computed(nth_tag(d, "h1", 0));
    note(st && st->font_size == 28 && st->font_weight == 700, N("h1 UA size+bold"));
    st = uw_computed(nth_tag(d, "head", 0));
    note(st && st->display == UW_DISP_NONE, N("head display:none"));

    st = uw_computed(uw_get_element_by_id(d, "lead"));
    note(st && st->font_size == 20, N("#lead id selector"));
    note(st && st->color.r == 0xc8 && st->color.g == 0x1e && st->color.b == 0x28,
         N(".hot class color"));
    note(st && st->font_weight == 700, N(".hot bold"));
    note(st && st->margin[UW_TOP].unit == UW_LEN_PX
            && st->margin[UW_TOP].v == 12
            && st->margin[UW_LEFT].v == 0, N("#lead margin shorthand"));
    note(st && st->text_align == UW_ALIGN_CENTER, N("p tag rule"));

    st = uw_computed(nth_tag(d, "p", 1));
    note(st && st->color.r == 0x12 && st->color.g == 0x34 && st->color.b == 0x56,
         N("style= attribute"));

    st = uw_computed(nth_tag(d, "li", 0));
    note(st && st->color.r == 0x28 && st->color.g == 0x5a && st->color.b == 0x1e,
         N("ul li descendant"));
    note(st && st->display == UW_DISP_LIST_ITEM && st->list_bullet == 1,
         N("li list-item+disc"));

    st = uw_computed(nth_tag(d, "a", 0));
    note(st && st->underline, N("a underline (UA)"));

    st = uw_computed(nth_tag(d, "pre", 0));
    note(st && st->font_family == UW_FF_MONO && st->white_space == UW_WS_PRE,
         N("pre mono+pre (UA)"));
#undef N
}

int main(void)
{
    uw_doc *d;
    uw_node *n;
    int els = 0, agree = 1;

    /* snapshot of the built-in cascade's core fields, element by element */
    struct snap { unsigned char display; int fw, fs; uw_color col;
                  unsigned char ul; } snaps[64];

    d = uw_parse_string(HTML, sizeof HTML - 1, NULL);
    if (!d) { printf("FAIL parse\n"); return 1; }

    /* ---- pass 1: the built-in cascade -------------------------------- */
    uw_add_inline_sheets(d);
    uw_style_document(d, 640, 400);
    check_expectations(d, "builtin");

    for (n = uw_next_in_order(uw_document(d), uw_document(d)); n && els < 64;
         n = uw_next_in_order(n, uw_document(d))) {
        const uw_style *st;
        if (uw_type(n) != UW_NODE_ELEMENT) continue;
        st = uw_computed(n);
        if (!st) continue;
        snaps[els].display = st->display;
        snaps[els].fw = st->font_weight;
        snaps[els].fs = st->font_size;
        snaps[els].col = st->color;
        snaps[els].ul = st->underline;
        els++;
    }

    /* ---- pass 2: the libcss bridge ------------------------------------ */
    uwx_libcss_register();
    note(uw_cascade_active(), "bridge registered");
    uw_style_document(d, 640, 400);
    note(uwx_libcss_status()[0] == 0, "bridge pass clean (no fallback)");
    check_expectations(d, "libcss");

    /* libcss-only ground: attribute selector */
    {   const uw_style *st = uw_computed(nth_tag(d, "a", 0));
        note(st && st->white_space == UW_WS_NOWRAP, "libcss: a[href] attr selector");
    }

    /* ---- parity across every element ---------------------------------- */
    {   int i = 0, mism = 0;
        for (n = uw_next_in_order(uw_document(d), uw_document(d)); n && i < els;
             n = uw_next_in_order(n, uw_document(d))) {
            const uw_style *st;
            if (uw_type(n) != UW_NODE_ELEMENT) continue;
            st = uw_computed(n);
            if (!st) continue;
            if (st->display != snaps[i].display || st->font_weight != snaps[i].fw
                || st->font_size != snaps[i].fs || st->underline != snaps[i].ul
                || st->color.r != snaps[i].col.r || st->color.g != snaps[i].col.g
                || st->color.b != snaps[i].col.b) {
                printf("   parity mismatch on element %d (%s): "
                       "disp %d/%d fw %d/%d fs %d/%d ul %d/%d "
                       "col %02x%02x%02x/%02x%02x%02x\n",
                       i, uw_tag_name(d, n),
                       snaps[i].display, st->display, snaps[i].fw,
                       st->font_weight, snaps[i].fs, st->font_size,
                       snaps[i].ul, st->underline,
                       snaps[i].col.r, snaps[i].col.g, snaps[i].col.b,
                       st->color.r, st->color.g, st->color.b);
                mism++;
            }
            i++;
        }
        agree = mism == 0;
    }
    note(agree, "parity: both cascades agree on core fields");

    /* the fallback path: unregister must restore the built-in cascade */
    uwx_libcss_unregister();
    note(!uw_cascade_active(), "unregister restores built-in");

    uw_doc_free(d);
    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail != 0;
}
